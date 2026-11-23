"""macOS native acceptance. Two private loopback servers, real IPC handlers, fresh app process per step."""
from contextlib import ExitStack
from email import policy
from email.parser import BytesParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
import ssl
import stat
import subprocess
import sys
import tempfile
import threading
import wave

PCM = Path(__file__).with_name("speech-probe.s16le").read_bytes()


def tls_context(directory):
    config = directory / "test.cnf"
    config.write_text("""[req]
prompt=no
distinguished_name=dn
[dn]
CN=Temporary speech fixture CA
[root]
basicConstraints=critical,CA:TRUE
keyUsage=critical,keyCertSign
[leaf]
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1
""")
    def openssl(*args):
        subprocess.run(["openssl", *args], cwd=directory, check=True, timeout=15,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    openssl("req", "-x509", "-nodes", "-newkey", "rsa:2048", "-days", "1", "-config", str(config),
            "-extensions", "root", "-keyout", "root.key", "-out", "ca.pem")
    openssl("req", "-new", "-nodes", "-newkey", "rsa:2048", "-config", str(config),
            "-subj", "/CN=localhost", "-keyout", "leaf.key", "-out", "leaf.csr")
    openssl("x509", "-req", "-days", "1", "-in", "leaf.csr", "-CA", "ca.pem", "-CAkey", "root.key",
            "-set_serial", "2", "-extfile", str(config), "-extensions", "leaf", "-out", "leaf.pem")
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(directory / "leaf.pem", directory / "leaf.key")
    return ctx


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        try:
            path, key, model, voice = self.server.expected.pop(0)
            assert self.path == path, "wrong operation or provider URL prefix"
            assert self.headers["Host"] == self.server.authority, "wrong provider address"
            assert self.headers.get_all("authorization") == ["Bearer " + key], "wrong provider key"
            n = int(self.headers["Content-Length"])
            assert 0 < n <= 512 * 1024
            data = self.rfile.read(n)
            if voice is None:
                mime = BytesParser(policy=policy.default).parsebytes(
                    ("Content-Type: "+self.headers["Content-Type"]+"\r\n\r\n").encode()+data)
                parts = {p.get_param("name", header="content-disposition"):p for p in mime.iter_parts()}
                assert parts["model"].get_payload(decode=True).decode() == model, "wrong transcription model"
                with wave.open(io.BytesIO(parts["file"].get_payload(decode=True))) as wav:
                    assert wav.getparams()[:3] == (1, 2, 16000)
                    assert wav.readframes(wav.getnframes()) == PCM, "wrong WAV input"
                answer = b'{"text":"Two providers."}'
                content_type = "application/json"
            else:
                body = json.loads(data)
                if self.server.probe:
                    assert body["input"].strip()
                else:
                    assert body["input"] == "Two providers."
                assert body == dict(model=model, input=body["input"], voice=voice, response_format="wav"), "wrong synthesis model, voice or format"
                buf = io.BytesIO()
                with wave.open(buf, "wb") as wav:
                    wav.setparams((1,2,16000,0,"NONE",""))
                    wav.writeframes(b"\x34\x12"*200)
                answer, content_type = buf.getvalue(), "audio/wav"
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(answer)))
            self.end_headers()
            self.wfile.write(answer)
        except Exception as error:
            # Deliberately omit received credentials and body from test output.
            reason = str(error) if isinstance(error, AssertionError) and str(error) in {
                "wrong operation or provider URL prefix", "wrong provider address", "wrong provider key",
                "wrong transcription model", "wrong WAV input", "wrong synthesis model, voice or format",
            } else type(error).__name__
            self.server.failures.append((reason, self.server.authority))
            self.send_error(400, "Speech fixture mismatch")


def start_server(stack, tls=None):
    server = stack.enter_context(ThreadingHTTPServer(("127.0.0.1", 0), Handler))
    server.expected, server.failures, server.probe = [], [], False
    host = "127.0.0.1" if tls is None else "localhost"
    server.authority = f"{host}:{server.server_port}"
    if tls:
        server.socket = tls.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    stack.callback(thread.join, 5)
    stack.callback(server.shutdown)
    scheme = "https" if tls else "http"
    return server, f"{scheme}://{server.authority}"


def main(binary, test_name):
    with tempfile.TemporaryDirectory(prefix="pet-speech-custom-") as tmp, ExitStack() as stack:
        root = Path(tmp)
        a, a_origin = start_server(stack)
        t, t_origin = start_server(stack, tls_context(root))
        a_url, t_url = a_origin+"/recognition/v1", t_origin+"/synthesis/v1"
        a_key, t_key = "fixture-secret-ASR-initial", "fixture-secret-TTS-initial"
        env = {key:os.environ[key] for key in ("PATH","LANG","TMPDIR","LC_ALL") if key in os.environ}
        env["PYTHONDONTWRITEBYTECODE"]="1"
        only = os.environ.get("PET_CUSTOM_SPEECH_CASE")
        if only and only not in {"fresh","tts-first","old-shared","old-separate","old-same-address-separate",
                                 "unified-shared","unified-separate","unified-same-address-override","unified-same-address-map",
                                 "plain-old-separate"}:
            raise ValueError("unknown speech fixture case")
        def step(case, action, **fields):
            if only and case != only:
                return
            directory = root/("case-"+case)
            directory.mkdir(exist_ok=True,mode=0o700)
            (root/"step.json").write_text(json.dumps(dict(action=action, **fields)))
            result = subprocess.run([binary,"--exact",test_name], env={**env,"PET_CUSTOM_SPEECH_FIXTURE":str(directory)},
                                    capture_output=True, timeout=25)
            assert not (a.failures or t.failures), f"local receiver validation failed in {case}/{action}: {a.failures+t.failures}"
            if result.returncode:
                text = (result.stdout+result.stderr).decode(errors="replace")
                for key in (a_key,t_key,"fixture-secret-ASR-rotated","fixture-secret-TTS-rotated"):
                    text = text.replace(key,"[fixture]")
                raise RuntimeError(f"{case}/{action}\n{text}")
            assert b"1 passed; 0 failed" in result.stdout, "native scenario did not run exactly once"
            if case == "plain-old-separate" and action == "run":
                for name in ("volcengine-device-asr.json", "voice-chat-settings.json"):
                    path = directory/name
                    assert json.loads(path.read_text())["format"] == "pet-manager-aes256gcm-v1"
                    assert stat.S_IMODE(path.stat().st_mode) == 0o600, "migrated credential file mode changed"
            assert not (a.expected or t.expected), f"missing network request in {case}/{action}"
            print(f"PASS {case}/{action}",flush=True)
        def run(case, ak, tk, *, am="model-asr", tm="model-tts", voice="fixture-voice", shared=False):
            if only and case != only:
                return
            a.expected.append(("/recognition/v1/audio/transcriptions", ak, am, None))
            (a if shared else t).expected.append(("/recognition/v1/audio/speech" if shared else "/synthesis/v1/audio/speech",
                                                tk, tm, voice))
            step(case,"run",asrUrl=a_url,ttsUrl=a_url if shared else t_url,asrModel=am,ttsModel=tm,voice=voice)
        def legacy(case, ak, tk, shared, unified=False):
            if only and case != only:
                return
            old={
                "asr":{"version":1,"provider":"openai","baseUrl":a_url,"model":"model-asr","apiKey":ak,
                       "resourceId":"volc.bigasr.sauc.duration"},
                "voice":{"ttsProvider":"openai","ttsBaseUrl":a_url if shared else t_url,
                         "ttsModel":"model-tts","ttsVoice":"fixture-voice","ttsApiKey":tk}
            }
            if unified:
                profiles=old.pop("asr")
                if tk and not case.endswith("override"):
                    profiles["legacyCustomKeys"]={a_url if shared else t_url:tk}
                    old["voice"]["ttsApiKey"]=""
                old["voice"]["speechProfiles"]=profiles
            if case == "plain-old-separate":
                step(case,"init")
                directory = root/("case-"+case)
                for name, fields in (("voice-chat-settings.json",old["voice"]),
                                     ("volcengine-device-asr.json",old["asr"])):
                    path = directory/name
                    with path.open("x") as output:
                        os.fchmod(output.fileno(),0o600)
                        json.dump(fields,output)
                return
            step(case,"init",legacy=old)
        def probe(stage,model,key,voice=None):
            if only and only != "fresh":
                return
            server = a if stage=="asr" else t
            path = "/recognition/v1/audio/transcriptions" if stage=="asr" else "/synthesis/v1/audio/speech"
            server.expected.append((path,key,model,voice))
            server.probe=True
            try:
                step("fresh","probe",stage=stage)
            finally:
                server.probe=False
        exercise(step,run,legacy,a_url,t_url,a_key,t_key,probe)


def exercise(step,run,legacy,a_url,t_url,a_key,t_key,probe):
    def save(case,stage,url,model,*,key=None,voice=None,reject=False):
        values=dict(stage=stage,baseUrl=url,model=model)
        if key is not None: values["apiKey"]=key
        if voice is not None: values["voice"]=voice
        step(case,"reject" if reject else "save",input=values)

    # Each run executes in a new OS process; both real wire requests are checked every time.
    case="fresh"
    step(case,"init")
    save(case,"asr",a_url,"model-asr",key=a_key)
    save(case,"tts",a_url,"model-tts",voice="fixture-voice",reject=True)
    save(case,"tts",t_url,"model-tts",key=t_key,voice="fixture-voice")
    run(case,a_key,t_key)
    rotated_asr,rotated_tts="fixture-secret-ASR-rotated","fixture-secret-TTS-rotated"
    save(case,"asr",a_url,"revised-asr",key=rotated_asr)
    run(case,rotated_asr,t_key,am="revised-asr")
    save(case,"tts",t_url,"revised-tts",key=rotated_tts,voice="second-voice")
    run(case,rotated_asr,rotated_tts,am="revised-asr",tm="revised-tts",voice="second-voice")
    save(case,"asr",t_url,"forbidden",reject=True)
    save(case,"tts",a_url,"forbidden",key="",reject=True)
    save(case,"asr",a_url+"/chat/completions","forbidden",reject=True)
    save(case,"tts",t_url+"/chat/completions","forbidden",key="",reject=True)
    run(case,rotated_asr,rotated_tts,am="revised-asr",tm="revised-tts",voice="second-voice")

    step(case,"preset")
    probe("asr","revised-asr",rotated_asr)
    probe("tts","revised-tts",rotated_tts,"second-voice")

    step("tts-first","init")
    save("tts-first","tts",t_url,"model-tts",key=t_key,voice="fixture-voice")
    save("tts-first","asr",t_url,"model-asr",key="",reject=True)
    save("tts-first","asr",a_url,"model-asr",key=a_key)
    run("tts-first",a_key,t_key)

    # Shared ASR credential with no TTS override, and two independently stored legacy keys.
    # A matching legacy URL is especially important: key identity must still include stage.
    for case,shared,independent,unified in [
        ("old-shared",True,False,False),
        ("old-separate",False,True,False),
        ("plain-old-separate",False,True,False),
        ("old-same-address-separate",True,True,False),
        ("unified-shared",True,False,True),
        ("unified-separate",False,True,True),
        ("unified-same-address-override",True,True,True),
        ("unified-same-address-map",True,True,True),
    ]:
        old_tts=t_key if independent else a_key
        legacy(case,a_key,t_key if independent else "",shared,unified)
        run(case,a_key,old_tts,shared=shared)
        save(case,"tts",a_url if shared else t_url,"new-tts",voice="new-voice")
        run(case,a_key,old_tts,shared=shared,tm="new-tts",voice="new-voice")
        save(case,"asr",a_url,"new-asr",key="")
        run(case,a_key,old_tts,shared=shared,am="new-asr",tm="new-tts",voice="new-voice")


if __name__=="__main__":
    main(*sys.argv[1:])
