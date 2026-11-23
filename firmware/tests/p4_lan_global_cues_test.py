"""Authenticated-LAN audio phase adapter with production handlers and real host filesystem."""
import base64
import json
import os
from pathlib import Path
import subprocess

from p4_audio_patch_test import FUNCTIONS, ROOT, TESTS, function


def build_driver(work, mbedcrypto=None):
    work.mkdir(exist_ok=True)
    (work / "esp_attr.h").write_text("#define EXT_RAM_BSS_ATTR\n")
    (work / "spiffs").mkdir(exist_ok=True)
    source = (ROOT / "main/pet_p4_protocol.c").read_text()
    code = "\n".join(function(source, n) for n in FUNCTIONS)
    code += '\n#include "pet_p4_protocol_audio_patch.inc"\n#include "pet_p4_json_source.h"\n'
    code += '''#include "pet_p4_raw_assets.h"
static void handle_asset_topic(pet_p4_runtime_state_t *s,const char *topic,const cJSON *p,
        pet_p4_send_line_fn send,void *route){
      assert(!strncmp(topic,"asset/audio-",12));handle_audio_patch_topic(s,topic,p,send,route);
    }\n'''
    code += function(source, "pet_p4_handle_lan_appearance")
    fixture = (TESTS / "p4_audio_patch_protocol_test.c.in").read_text().split("int main(void)")[0]
    path = work / "driver.c"
    path.write_text(fixture.replace("/* PRODUCTION_PROTOCOL */",code) + (TESTS / "p4_lan_global_cues_driver.inc").read_text())
    exe = work / "driver"
    subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",
        "-D_POSIX_C_SOURCE=200809L","-DPET_P4_SYSTEM_WIFI=1",'-DPET_AUDIO_PATCH_FS_PREFIX="spiffs"',
        "-I",work,"-I",ROOT/"main","-I",TESTS/"vendor/cjson",path,ROOT/"main/pet_p4_audio_patch.c",
        ROOT/"main/pet_p4_resources.c",TESTS/"vendor/cjson/cJSON.c",
        *(["-DPET_TEST_REAL_BASE64",mbedcrypto] if mbedcrypto else []),"-lm","-o",exe],check=True)
    return exe


def test_wlan_global_writer_binding_abort_disconnect(tmp_path):
    exe = build_driver(tmp_path)
    with subprocess.Popen([exe],cwd=tmp_path,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True) as proc:
        target=dict(boardDeviceId="fixture-board",bootId="0123456789abcdef",transferId="fixture",packId="0"*64,slot=0,scope="global")
        def call(phase,**fields):
            proc.stdin.write(f"{phase} "+json.dumps(target | fields)+"\n");proc.stdin.flush()
            return json.loads(proc.stdout.readline())
        empty=dict(files=[],totalBytes=0)
        assert call(6,scope="other",**empty) is None
        assert call(6,boardDeviceId="other",**empty)["ok"] is False
        assert call(6,bootId="other",**empty)["ok"] is False
        assert call(6,packId="1"*64,**empty)["ok"] is False
        assert call(6,**empty)["ok"]
        assert call(8,files=1,totalBytes=0) is None
        assert call(9,transferId="other")["ok"] is False
        assert call(9)["ok"]
        assert not list((tmp_path/"spiffs").glob("*.idx"))
        wave=base64.b64decode("UklGRiYAAABXQVZFZm10IBAAAAABAAEAgD4AAAB9AAACABAAZGF0YQIAAAABAA==")
        checksum=14695981039346656037
        for b in wave:checksum=((checksum ^ b)*1099511628211) % (1<<64)
        item=dict(family="done",path="p4/audio/done.wav",size=len(wave),checksum=f"{checksum:016x}")
        assert call(6,files=[item],totalBytes=len(wave))["ok"]
        chunk=dict(path=item["path"],index=0,offset=0,data=base64.b64encode(wave).decode())
        assert call(7,**(chunk | {"offset":1}))["ok"] is False
        ack=call(7,**chunk)
        assert (ack["ok"],ack["nextIndex"],ack["receivedBytes"])==(True,1,len(wave))
        ack=call(8,files=1,totalBytes=len(wave))
        assert ack["ok"] and ack["audioGeneration"]==1
        original={p.name:p.read_bytes() for p in (tmp_path/"spiffs").glob("*")}
        assert call(6,**empty)["ok"]
        call(99)  # Idle cleanup after the route is gone operates on staging only.
        assert call(8,files=0,totalBytes=0)["ok"] is False
        assert original=={p.name:p.read_bytes() for p in (tmp_path/"spiffs").glob("*")}
        proc.stdin.close();assert proc.wait(timeout=5)==0


if __name__=="__main__":
    import sys
    build_driver(Path(sys.argv[1]),Path(os.environ["PETUI_HOST_MBEDTLS_BUILD"])/"library/libmbedcrypto.a")
