"""Run the native save, probe and runtime in separate processes against a local HTTPS fixture."""
import base64
import io
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import wave


def certificate(directory):
    config = directory / "openssl.cnf"
    config.write_text("""[req]
prompt=no
distinguished_name=dn
[dn]
CN=Speech fixture
[root]
basicConstraints=critical,CA:TRUE
keyUsage=critical,keyCertSign
[leaf]
basicConstraints=critical,CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:api.xiaomimimo.com,DNS:one.invalid,DNS:two.invalid
""")
    def run(*args):
        subprocess.run(["openssl", *args], cwd=directory, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    run("req", "-x509", "-nodes", "-newkey", "rsa:2048", "-days", "1", "-config",
        str(config), "-extensions", "root", "-keyout", "root.key", "-out", "ca.pem")
    run("req", "-new", "-nodes", "-newkey", "rsa:2048", "-config", str(config),
        "-subj", "/CN=api.xiaomimimo.com", "-keyout", "leaf.key", "-out", "req.pem")
    run("x509", "-req", "-days", "1", "-in", "req.pem", "-CA", "ca.pem", "-CAkey",
        "root.key", "-set_serial", "2", "-extfile", str(config), "-extensions", "leaf", "-out", "leaf.pem")
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(directory / "leaf.pem", directory / "leaf.key")
    return ctx


def headers(stream):
    result = {}
    while True:
        line = stream.readline(8192)
        if line == b"\r\n":
            return result
        name, value = line.decode("ascii").split(":", 1)
        result[name.lower()] = value.strip()


def serve(sock, ctx, model, voice=None, key="offline-mimo-fixture", host="api.xiaomimimo.com"):
    with sock:
        sock.settimeout(8)
        with sock.makefile("rb") as raw:
            assert raw.readline(256).strip() == f"CONNECT {host}:443 HTTP/1.1".encode()
            headers(raw)
        sock.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
        with ctx.wrap_socket(sock, server_side=True) as tls, tls.makefile("rb") as stream:
            assert stream.readline(256).strip() == b"POST /v1/chat/completions HTTP/1.1"
            fields = headers(stream)
            assert fields["host"] == host
            assert fields["authorization"] == f"Bearer {key}"
            length = int(fields["content-length"])
            assert 0 < length < 512 * 1024
            body = json.loads(stream.read(length))
            assert body["model"] == model and body["stream"] is False
            if model == "mimo-v2.5-tts-voicedesign":
                design, message = body["messages"]
                assert design == {"role": "user", "content": voice}
                assert body["audio"] == {"format": "wav"}
            else:
                message, = body["messages"]
            if host == "two.invalid":
                assert "scoped-description" not in json.dumps(body)
            if voice is None:
                assert message["role"] == "user"
                part, = message["content"]
                assert part["type"] == "input_audio"
                prefix, data = part["input_audio"]["data"].split(",", 1)
                assert prefix == "data:audio/wav;base64"
                with wave.open(io.BytesIO(base64.b64decode(data, validate=True))) as wav:
                    assert wav.getparams()[:3] == (1, 2, 16000)
                    pcm = wav.readframes(wav.getnframes())
                    assert pcm == Path(__file__).with_name("speech-probe.s16le").read_bytes()
                answer = {"content": "Speech test."}
            else:
                assert message["role"] == "assistant" and message["content"]
                if model != "mimo-v2.5-tts-voicedesign":
                    assert body["audio"] == {"voice": voice, "format": "wav"}
                output = io.BytesIO()
                with wave.open(output, "wb") as wav:
                    wav.setparams((1, 2, 16000, 0, "NONE", ""))
                    wav.writeframes(b"\x34\x12" * 200)
                answer = {"audio": {"data": base64.b64encode(output.getvalue()).decode()}}
            data = json.dumps({"choices": [{"message": answer}]}).encode()
            tls.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n"
                        + f"Content-Length: {len(data)}\r\n\r\n".encode() + data)


def main():
    binary, name = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="pet-mimo-loopback-") as tmp, socket.socket() as listener:
        ctx = certificate(Path(tmp))
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        listener.settimeout(30)
        failures = []
        def requests():
            try:
                for expected in [
                    ("mimo-v2.5-asr",), ("mimo-v2.5-tts", "mimo_default"),
                    ("edited-asr",), ("edited-tts", "茉莉"),
                    ("edited-asr",), ("edited-tts", "runtime-pet"), ("edited-tts", "茉莉"),
                    ("edited-tts", "pet-specific"), ("edited-tts", "pet-specific"), ("edited-tts", "茉莉"),
                    ("mimo-v2.5-tts-voicedesign", "scoped-description", "test-one", "one.invalid"),
                    ("mimo-v2.5-tts-voicedesign", "global-other", "test-two", "two.invalid"),
                    ("mimo-v2.5-tts-voicedesign", "scoped-description", "test-one", "one.invalid"),
                    ("environment-asr", None, "offline-environment-key"),
                ]:
                    serve(listener.accept()[0], ctx, *expected)
            except BaseException as exc:
                failures.append(exc)
        thread = threading.Thread(target=requests, daemon=True)
        thread.start()
        for stage in ["legacy", "save", "probe", "edit", "restore", "probe", "runtime", "persona", "persona-restart", "custom-persona", "env-default", "env-runtime", "reset"]:
            env = {**os.environ, "PET_MIMO_LOOPBACK_DIR": tmp, "PET_MIMO_STAGE": stage,
                   "PET_MIMO_PROXY_PORT": str(listener.getsockname()[1])}
            for var in ["HACHIMO_ASR_PROVIDER", "HACHIMO_ASR_MODEL", "HACHIMO_MIMO_SPEECH_API_KEY", "VOLCENGINE_ASR_API_KEY"]:
                env.pop(var, None)
            if stage.startswith("env-"):
                env.update(HACHIMO_ASR_PROVIDER="mimo", HACHIMO_MIMO_SPEECH_API_KEY="offline-environment-key")
                if stage == "env-runtime":
                    env["HACHIMO_ASR_MODEL"] = " environment-asr "
            result = subprocess.run([binary, "--exact", name], env=env, timeout=30)
            if failures:
                raise failures[0]
            result.check_returncode()
        thread.join(2)
        assert not thread.is_alive() and not failures


if __name__ == "__main__":
    main()
