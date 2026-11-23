"""Native firmware worker, catalog crypto, privilege gate and production store/commit/delete.

Only the radio/TLS, scheduler, flash, and unrelated runtime facilities use the
existing host adapters. Optional HIL service probing uses a real HTTPS client.
"""
import base64
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import ssl
import struct
import subprocess
import sys
from urllib.parse import parse_qs, urlsplit
import urllib.request

import pytest
from p4_render_trace_test import function
from petui_storage_mutation_test import _mutation_source

FW = Path(__file__).resolve().parents[1]
MAIN = FW / "main"
TESTS = FW / "tests"
CJSON = TESTS / "vendor/cjson"
WORK = FW.parents[1]
GET = C.CFUNCTYPE(C.c_int, C.c_char_p, C.c_void_p, C.c_size_t)


class View(C.Structure):
    _fields_ = [("title", C.c_char * 97), ("version", C.c_char * 33), ("id", C.c_char * 44), ("status", C.c_char_p),
                ("index", C.c_uint), ("count", C.c_uint), ("privileges", C.c_uint),
                ("busy", C.c_bool), ("more", C.c_bool), ("install", C.c_bool),
                ("remove", C.c_bool), ("installed", C.c_bool), ("added_save", C.c_bool)]


def compact(obj):
    return json.dumps(obj, separators=(",", ":")).encode()


def crypto(tmp):
    default = WORK / "toolchains/platformio/v3/packages/framework-espidf/components/mbedtls/mbedtls"
    sdk = Path(os.environ.get("PETUI_HOST_MBEDTLS_SOURCE", default))
    build = Path(os.environ.get("PETUI_HOST_MBEDTLS_BUILD", tmp / "mbedtls"))
    if not (build / "library/libmbedcrypto.a").is_file():
        subprocess.run(["cmake", "-S", sdk, "-B", build, "-DENABLE_TESTING=OFF",
                        "-DENABLE_PROGRAMS=OFF", "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"], check=True)
        subprocess.run(["cmake", "--build", build, "--target", "mbedcrypto", "-j4"], check=True)
    return ["-I", sdk / "include"], [build / "library/libmbedcrypto.a", *build.glob("3rdparty/*/*.a")]


@pytest.fixture(scope="module")
def host_crypto(tmp_path_factory):
    return crypto(tmp_path_factory.mktemp("community-crypto"))


def native(tmp, pub, dependencies=None, config=None):
    inc, libs = dependencies or crypto(tmp)
    (tmp/"esp_attr.h").write_text("#define EXT_RAM_BSS_ATTR\n")
    (tmp/"esp_heap_caps.h").write_text("#include <stdlib.h>\n#define heap_caps_calloc(n,s,c) calloc(n,s)\n#define heap_caps_malloc(n,c) malloc(n)\n")
    (tmp/"esp_random.h").write_text("#include <stddef.h>\nvoid esp_fill_random(void *,size_t);\n")
    (tmp/"hooks.h").write_text("#define pdPASS 1\nint xTaskCreate(void(*)(void*),const char*,unsigned,void*,unsigned,void*);\nvoid vTaskDelete(void*);\n")
    network=(MAIN/"pet_p4_wifi_data.c").read_text()
    (tmp/"community_https_under_test.inc").write_text("\n".join(function(network,name) for name in
        ("tls_waitable","request_yield","tls_read_exact","tls_send_request","pet_community_https")))
    code = (MAIN/"pet_p4_miniapp.c").read_text()
    model = _mutation_source(False).split("int main(void){")[0]
    model = model.replace(function(model,"catalog_first_free_slot"),function(code,"catalog_first_free_slot"))
    model = model.replace("widget_json[512],buttons_json[512]", "widget_json[4096],buttons_json[2048]")
    model = model.replace("pet_p4_component_metadata_t package_metadata;", "pet_p4_component_metadata_t package_metadata;char community_save[65];")
    model += '''
#include "pet_p4_community.h"
static const size_t pet_p4_builtin_slice_count=1;
static const char pet_p4_builtin_index_json[]="{\\"components\\":[{\\"id\\":\\"protected-tool\\"}]}";
static bool pet_p4_miniapp_storage_busy(void){return false;}
'''
    for name in ("safe_id", "pet_p4_miniapp_lan_install_idle", "pet_p4_miniapp_install_begin",
                 "pet_p4_miniapp_remove_preserving_save"):
        model += function(code, name)
    model += '''
#include "pet_p4_community_store.inc"
void model_start(void){
  assert(pet_storage_lane_configure(&g_storage_lane,unlock,lock,owner,NULL));
  reset();widget_install_stage_reset();
}
bool model_row(size_t i,char *id,char *title,char *version,bool *owned,bool *persist){
  if(i>=g_catalog_count)return false;
  strcpy(id,g_catalog[i].widget_id);strcpy(title,g_catalog[i].title);
  strcpy(version,g_catalog[i].package_metadata.version);
  *owned=g_catalog[i].community_save[0]!=0;
  *persist=strlen(g_catalog[i].community_save)==64;
  return true;
}
size_t pet_p4_miniapp_catalog_count(void){return g_catalog_count;}
void model_lane(bool begin){
  if(begin) assert(pet_storage_lane_enter(&g_storage_lane)); else assert(pet_storage_lane_leave(&g_storage_lane));
}
void model_rotate_owner(const char *id){
  int i=catalog_find(g_catalog,g_catalog_count,id);assert(i>=0);
  static char original;
  if (original) {g_catalog[i].community_save[0]=original;original=0;}
  else {original=g_catalog[i].community_save[0];g_catalog[i].community_save[0]=original=='0'?'1':'0';}
}
unsigned model_commits(void){return commits;}
unsigned model_forgets(void){return forgets;}
void model_fault(int stage){fail_io=stage?io_calls+stage:0;}
'''
    (tmp/"model.c").write_text(model)
    public_flags = ['-DPET_COMMUNITY_KEY_B64="'+pub+'"']
    if config:
        from community_build_config import generate
        header = tmp / "community-public.h"
        generate(header,str(config))
        public_flags = ["-include",header]
    lib = tmp / "community.so"
    subprocess.run(["cc", "-shared", "-fPIC", "-std=c11", "-DPET_P4_SYSTEM_WIFI=1",
                    *public_flags, "-include", tmp/"hooks.h",
                    "-I", tmp, "-I", MAIN, "-I", CJSON, "-I", TESTS/"audio_volume_stubs",
                    *inc, tmp/"model.c", TESTS/"p4_community_device_host.c",
                    *(MAIN/("pet_p4_"+unit+".c") for unit in ("community","community_profile","community_crypto","storage_lane")),
                    CJSON/"cJSON.c", *libs, "-o", lib], check=True)
    api = C.CDLL(str(lib))
    api.test_set_http.argtypes=[GET]
    api.pet_community_view.argtypes=[C.POINTER(View)]
    api.model_start()
    return api


def view(api):
    state = View()
    api.pet_community_view(C.byref(state))
    return state


def frame(widget, buttons):
    chunks = [(b"buttons.json", compact(buttons)), (b"runtime/widget.json", compact(widget))]
    return b"".join(path+b"\0"+struct.pack("<I", len(blob))+blob for path,blob in chunks)


def fixture(count, persist=False):
    action = "local_up" if count else "local_flip"
    buttons = [{"action":action,"event":"button.sw1.short_press","control":"SW1","label":"Go"}]
    widget = {"schema_version":1,"engine":"p4-bounded-runtime-v4","initial_state":"ready",
              "states":["ready"] if count else ["ready","rest"],"vars":{},"tick":[],
              "dashboard":{"title":"Tally" if count else "Focus",
                           "headline":"Count" if count else {"switch_state":{"ready":"Focus","rest":"Rest"}}},
              "transitions":[{"on":action,"from":"ready","inc":{"value":1}}] if count else
                            [{"on":action,"from":"ready","to":"rest"},{"on":action,"from":"rest","to":"ready"}]}
    if count:
        widget["vars"]={"value":{"type":"int","init":0}}
        widget["surface"]={"engine":"p4-app-surface-v1","graphics":"p4-app-graphics-v1","background":0,
                           "elements":[{"type":"text","x":10,"y":12,"w":88,"h":40,"color":255,"bind":"headline"}]}
    if persist:
        widget["persist"]={"version":1,"vars":["value"],"states":{"ready":"ready"}}
    return widget,buttons


class Service:
    def __init__(self,tmp):
        self.key=tmp/"test.key"
        subprocess.run(["openssl","ecparam","-name","prime256v1","-genkey","-noout","-out",self.key],check=True)
        self.public=base64.b64encode(subprocess.check_output(["openssl","pkey","-in",self.key,"-pubout","-outform","DER"])).decode()
        self.packages={}
        self.edit=lambda data: None
        self.damage_signature=False
        self.corrupt=False
        self.requests=[]

    def publish(self,id,version,data,grant=0):
        sha=hashlib.sha256(data).hexdigest()
        item=dict(id=id,name=id,version=version,sourceSha256=sha,sha256=sha,downloadBytes=len(data),
                  installBytes=len(data)-41,fileCount=2,privileges=grant)
        self.packages[id]=(item,data)

    def get(self,path):
        self.requests.append(path)
        if path.startswith("/api/v1/device/catalog?"):
            query=parse_qs(urlsplit(path).query,keep_blank_values=True)
            after=query["after"][0]
            rows=[self.packages[i][0] for i in sorted(self.packages) if i>after]
            data=dict(schema=1,board="wifi6-p4",profile="p4-local-v1",nonce=query["nonce"][0],after=after,
                      next=rows[3]["id"] if len(rows)>4 else "",items=rows[:4])
            self.edit(data)
            raw=compact(data)
            sig=subprocess.check_output(["openssl","dgst","-sha256","-sign",self.key],input=raw)
            if self.damage_signature: sig=sig[:-1]+bytes([sig[-1]^1])
            return compact(dict(data=base64.b64encode(raw).decode(),signature=base64.b64encode(sig).decode()))
        prefix="/api/v1/device/payload/wifi6-p4/"
        assert path.startswith(prefix)
        id,version,digest=path[len(prefix):].split("/")
        item,raw=self.packages[id]
        if (item["version"],item["sha256"])!=(version,digest): return b""
        return raw[:-1]+b"X" if self.corrupt else raw


def connect(api, get):
    errors=[]
    @GET
    def callback(path,out,max_len):
        try:
            raw=get(path.decode())
            if len(raw)>max_len: return -1
            C.memmove(out,raw,len(raw))
            return len(raw)
        except Exception as exc:
            errors.append(exc)
            return -1
    api.test_set_http(callback)
    return callback,errors


def flush(api,admit=True):
    api.test_fetch()
    api.test_dispatch(admit)


def refresh(api):
    api.pet_community_browse(False)
    flush(api)


def test_signed_lifecycle_and_failures(tmp_path,host_crypto):
    service=Service(tmp_path)
    api=native(tmp_path,service.public,host_crypto)
    callback,errors=connect(api,service.get)
    switch=frame(*fixture(False))
    counter=frame(*fixture(True))
    service.publish("focus-switch","1.0.0",switch)
    service.publish("local-tally","1.0.0",counter)
    api.pet_community_run(None,True)
    api.pet_community_browse(False)
    api.pet_community_run(C.c_void_p(1),True)
    assert view(api).busy
    flush(api)
    assert view(api).count==2 and view(api).title==b"focus-switch"
    api.pet_community_action(False)
    flush(api)
    assert api.model_commits()==1 and view(api).remove and not view(api).install
    api.pet_community_move(1)
    api.pet_community_action(False)
    flush(api)
    assert api.model_commits()==2
    service.publish("local-tally","1.1.0",frame(*fixture(True,True)),1)
    refresh(api)
    api.pet_community_move(1)
    assert view(api).privileges==1 and view(api).added_save and view(api).install
    api.model_rotate_owner(b"local-tally")
    api.pet_community_action(False)
    flush(api)
    assert api.model_commits()==2  # a different trust root cannot inherit an ID
    api.model_rotate_owner(b"local-tally")
    api.pet_community_action(False)
    flush(api,admit=False)
    assert api.model_commits()==2 and view(api).added_save
    api.model_fault(1)
    api.pet_community_action(False)
    flush(api)
    assert api.model_commits()==2 and view(api).install
    api.model_fault(0)
    api.pet_community_action(False)
    flush(api)
    assert api.model_commits()==3 and not view(api).install
    service.publish("local-tally","1.2.0",counter,0)
    refresh(api)
    api.pet_community_move(1)
    api.pet_community_action(False)
    flush(api)
    assert api.model_commits()==3  # dropping an existing save contract fails closed
    changed,buttons=fixture(True,True)
    changed["persist"]["version"]=2
    service.publish("local-tally","1.2.0",frame(changed,buttons),1)
    refresh(api)
    api.pet_community_move(1)
    api.pet_community_action(False)
    flush(api)
    assert api.model_commits()==3
    service.packages.clear()
    refresh(api)
    api.test_online(False)
    api.pet_community_local()
    assert view(api).count==2
    if view(api).id!=b"local-tally": api.pet_community_move(1)
    assert view(api).id==b"local-tally"
    api.pet_community_action(True)
    flush(api)
    assert api.model_commits()==4 and view(api).count==1 and api.model_forgets()==0
    api.test_online(True)

    service.publish("other-app","2.0.0",switch)
    for edit in (lambda d:d.update(nonce="0"*32),lambda d:d.update(board="pico"),lambda d:d.update(after="old"),
                 lambda d:d["items"][0].update(privileges=4)):
        service.edit=edit
        refresh(api)
        assert not view(api).count
    service.edit=lambda d:None
    service.publish("other-app","2.0.0",switch)
    service.damage_signature=True
    refresh(api)
    assert not view(api).count
    service.damage_signature=False
    refresh(api)
    service.corrupt=True
    api.pet_community_action(False)
    flush(api)
    assert api.model_commits()==4
    service.corrupt=False
    for transport_failure in (1,2):
        api.test_wire_fault(transport_failure)
        api.pet_community_action(False)
        flush(api)
        assert api.model_commits()==4
    api.test_wire_fault(0)
    api.pet_community_action(False)
    api.test_fetch()
    api.pet_community_close()
    api.test_dispatch(True)
    assert api.model_commits()==4

    # Signed bytes still need a supported schema, exact declared authority,
    # complete frames, local ID ownership and an available storage lane.
    unsafe=fixture(False)
    unsafe[0]["host"]=True
    # A signed two-file profile cannot smuggle a launcher sidecar in a third frame.
    thumb_frame=b"runtime/center-thumb.p4t\0"+struct.pack("<I",1568)+bytes(1568)
    for payload,grant in ((frame(*unsafe),0),(counter+b"x",0),(frame(*fixture(True,True)),0),
                          (switch+thumb_frame,0),
                          (counter,1),(switch.replace(b"buttons.json",b"../save.json"),0)):
        service.publish("other-app","2.0.0",payload,grant)
        refresh(api)
        api.pet_community_action(False)
        flush(api)
        assert api.model_commits()==4 and view(api).install
    service.packages.clear()
    service.publish("protected-tool","2.0.0",switch)
    refresh(api)
    api.pet_community_action(False)
    flush(api)
    assert api.model_commits()==4
    service.packages.clear()
    service.publish("garden","2.0.0",switch)
    refresh(api)
    assert not view(api).install and not view(api).remove
    for i in range(5): service.publish("page-"+str(i),"1.0.0",switch)
    refresh(api)
    assert view(api).count==4 and view(api).more
    api.pet_community_browse(True)
    flush(api)
    assert view(api).count==2 and view(api).title==b"page-3" and not view(api).more
    assert not errors and callback


def probe_hil(tmp,config):
    # Explicitly invoked diagnostic for the isolated GET-only HIL server.
    sys.path.insert(0,str(FW/"tools"))
    from community_build_config import public_fields
    fields=public_fields(config)
    origin="https://%s:%d"%(fields["PET_COMMUNITY_HOST"],fields["PET_COMMUNITY_PORT"])
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}),
        urllib.request.HTTPSHandler(context=ssl.create_default_context(cadata=fields["PET_COMMUNITY_CA_PEM"])))
    api=native(tmp,fields["PET_COMMUNITY_KEY_B64"],config=config)
    def get(path):
        with opener.open(origin+path,timeout=10) as response:
            return response.read(8193)
    callback,errors=connect(api,get)
    refresh(api)
    assert not errors and view(api).count==2
    for i in range(2):
        v=view(api)
        assert v.install
        api.pet_community_action(False)
        flush(api)
        assert not errors and api.model_commits()==i+1 and view(api).remove
        print(v.title.decode(),v.version.decode(),"host commit",api.model_commits())
        api.pet_community_move(1)
    assert callback


def test_public_configuration_is_data_and_clears_cached_candidate(tmp_path):
    sys.path.insert(0,str(FW/"tools"))
    from community_build_config import generate
    key=Service(tmp_path).public
    fields={"PET_COMMUNITY_HOST":"board.test","PET_COMMUNITY_PORT":18443,"PET_COMMUNITY_KEY_B64":key}
    source=tmp_path/"trust.json"
    generated=tmp_path/"public.generated.h"
    source.write_text(json.dumps(fields))
    generate(generated,str(source))
    candidate=generated.read_text()
    assert key in candidate
    header=tmp_path/"candidate.h"
    header.write_text(candidate)
    generate(generated,str(header))
    assert candidate==generated.read_text()
    for bad in ('#include "/tmp/component.h"\n'+candidate,candidate+'#define EXTRA 1\n',
                candidate+'#define PET_COMMUNITY_PORT 443\n',candidate.replace("18443","(18443)"),
                candidate.replace("board.test",'board.test\\n#include unexpected'),
                candidate.replace(key,"broken")):
        header.write_text(bad)
        with pytest.raises((ValueError,TypeError)):
            generate(generated,str(header))
    with pytest.raises(ValueError):
        generate(generated,"relative/header.h")
    generate(generated,None)
    assert generated.read_text()=="#pragma once\n"


def test_public_default_and_offline_management(tmp_path,host_crypto):
    api=native(tmp_path,"",host_crypto)
    def forbidden(path):
        raise AssertionError(path)
    callback,errors=connect(api,forbidden)
    api.pet_community_browse(False)
    assert not view(api).busy and not view(api).count and "密钥" in view(api).status.decode()
    api.pet_community_local()
    assert view(api).installed and not view(api).count and not api.model_commits()
    assert callback and not errors


def test_two_local_packages_execute_real_runtime_actions(tmp_path,host_crypto):
    inc,libs=host_crypto
    default_wamr=next((WORK/"vendor/wamr").glob("*/core/iwasm/include/wasm_export.h"),None)
    wamr=os.environ.get("WAMR_ROOT")
    headers=Path(wamr)/"core/iwasm/include" if wamr else default_wamr.parent
    # Unreachable built-in bundle inflation still requires SDK declarations.
    (tmp_path/"miniz.h").write_text("""typedef int tinfl_decompressor;
typedef int tinfl_status;
typedef unsigned char mz_uint8;
#define TINFL_FLAG_PARSE_ZLIB_HEADER 1
#define TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF 2
#define TINFL_STATUS_DONE 0
""")
    (tmp_path/"esp_random.h").write_text("#include <stdint.h>\nuint32_t esp_random(void);\n")
    src=TESTS/"p4_community_runtime_host.c"
    args=["cc","-std=gnu11","-O1","-fvisibility=hidden","-ffunction-sections","-fdata-sections",
          "-Wno-implicit-function-declaration","-Wno-int-conversion","-Wno-unknown-attributes",
          "-I",tmp_path,"-I",TESTS/"audio_volume_stubs","-I",MAIN,"-I",CJSON,
          "-I",FW/"components/hachi_wamr","-I",headers,*inc]
    runtime=tmp_path/"runtime.o"
    subprocess.run([*args,"-c",src,"-o",runtime],check=True)
    exe=tmp_path/"dispatch"
    modules=("fruit_core","game","local_tools","side_scene","side_scene_json","sfx_core","surface")
    linker="-Wl,-dead_strip" if sys.platform=="darwin" else "-Wl,--gc-sections"
    subprocess.run([*args,linker,runtime,"-DCOMMUNITY_RUNTIME_ADAPTERS=1",src,
                    CJSON/"cJSON.c",*(MAIN/f"pet_p4_{m}.c" for m in modules),*libs,"-lm","-o",exe],check=True)
    for counter,persist in ((False,False),(True,False),(True,True)):
        w,b=fixture(counter,persist)
        if counter:
            w["dashboard"]["metricValue"]={"var":"value"}
        app_id="local-tally" if counter else "focus-switch"
        args=[exe,app_id,compact(w),compact(b)]
        result=subprocess.check_output([*args,"save"],cwd=tmp_path,text=True)
        rows=[line.split("|") for line in result.splitlines()]
        if counter:
            assert [r[1] for r in rows]==["0","1","2"]
        else:
            assert [r[2] for r in rows]==["Focus","Rest","Focus"]
        assert all(r[3]==str(int(persist)) for r in rows)
        saved=tmp_path/app_id
        assert saved.exists()==persist
        if persist:
            assert json.loads(saved.read_bytes())=={"version":1,"state":"ready","vars":{"value":2}}
            restored=subprocess.check_output([*args,"restore"],cwd=tmp_path,text=True)
            assert [line.split("|")[1] for line in restored.splitlines()]==["2","3","4"]
