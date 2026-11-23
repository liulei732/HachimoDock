"""Exercise the production clear journal against the installed SDK's RAM-backed SPIFFS."""
import base64
import hashlib
import json
import subprocess

from p4_catalog_zero_rewrite_test import FIXTURES, ROOT, sdk_source, writer_hosts
from p4_render_trace_test import function


def epoch_host_source():
    sdk = sdk_source()
    store = (ROOT / "main/pet_p4_app_store.c").read_text()
    vfs = (sdk.parents[1] / "esp_spiffs.c").read_text()
    # Reuse the NOR geometry, bitwise writes and locking from the SDK catalog fixture.
    nor = (FIXTURES / "catalog_zero_rewrite_host.c.in").read_text().split('#include "cJSON.h"')[0]
    code = (FIXTURES / "save_epoch_spiffs.c.in").read_text()
    mapping = "\n".join(function(vfs, name) for name in (
        "spiffs_res_to_errno", "vfs_spiffs_unlink", "vfs_spiffs_rename", "vfs_spiffs_fsync"))
    start = store.index("static void path_for(")
    end = store.index("static bool commit_forget_epoch", start)
    # Pin the ID digest independently, while exercising the production path encoding.
    ids = ["records-hil-" + "f" * 16, "records-hil-" + "f" * 15 + "0"]
    digests = []
    for name in ids:
        octets = ",".join(str(x) for x in hashlib.sha256(name.encode()).digest())
        digests.append('{"' + name + '", {' + octets + '}}')
    return (nor + code.replace("/* SDK_VFS */", mapping)
            .replace("/* ID_DIGESTS */", ",\n".join(digests))
            .replace("/* STORE */", function(store, "checksum") + "\n" + store[start:end]))


def run_epoch_host(hosts, name, source, count_lookups=False):
    work, _, env = hosts
    sdk = sdk_source()
    path = work / (name + ".c")
    path.write_text(source)
    app = work / name
    objects = [work / (n + ".c.o") for n in
               ("spiffs_cache", "spiffs_check", "spiffs_gc", "spiffs_hydrogen", "spiffs_nucleus")]
    cc = ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
          "-I", work / "include", "-I", sdk, "-I", ROOT / "main"]
    if count_lookups:
        instrumented = work / "counted-hydrogen.o"
        subprocess.run([*cc, "-fno-sanitize=alignment",
                        "-Dspiffs_object_find_object_index_header_by_name=tracked_lookup",
                        "-c", sdk / "spiffs_hydrogen.c", "-o", instrumented], env=env, check=True)
        objects[3] = instrumented
    subprocess.run([*cc, path, *objects, "-o", app], env=env, check=True)
    subprocess.run([app], env=env, check=True, timeout=90)


def test_epoch_paths_and_delete_reinstall_on_sdk_nor(writer_hosts):
    run_epoch_host(writer_hosts, "save_epoch", epoch_host_source())


def test_paired_journals_and_cold_recovery_after_each_flash_write(writer_hosts):
    store = (ROOT / "main/pet_p4_app_store.c").read_text()
    vfs = (sdk_source().parents[1] / "esp_spiffs.c").read_text()
    directory = "\n".join(function(vfs, name) for name in (
        "vfs_spiffs_opendir", "vfs_spiffs_closedir", "vfs_spiffs_readdir_r", "vfs_spiffs_readdir"))
    start = store.index("typedef struct {\n  pet_resource_lease_t lease;\n  int64_t deadline;")
    end = store.index("#if PET_COMPONENT_PROGRAM_ENABLED\n/* The package owner", start)
    code = epoch_host_source().split("int main(void){")[0]
    # Simulate loss immediately after a physical write, without orderly unmount.
    code = code.replace("static s32_t hal_write", "static void cut_after_write(void);\nstatic s32_t hal_write")
    code = code.replace("image[addr+i]&=in[i];}return", "image[addr+i]&=in[i];}cut_after_write();return")
    code = code.replace("memset(image+addr,255,n);return", "memset(image+addr,255,n);cut_after_write();return")
    extra = (FIXTURES / "save_forget_powercut.c.in").read_text()
    run_epoch_host(writer_hosts, "forget-cuts", code + extra.replace("/* DIRECTORY */", directory)
                  .replace("/* SCAN */", store[start:end]), count_lookups=True)

def test_old_names_remain_stable_at_the_actual_filesystem_boundaries(tmp_path):
    source = (ROOT / "main/pet_p4_app_store.c").read_text()
    paths = "\n".join(function(source, name) for name in ("path_for", "scoped_path_for", "epoch_path_for"))
    component = "a" * 47
    digest = hashlib.sha256(component.encode()).digest()
    backend = """
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define PET_APP_SAVE_ROOT "/spiffs"
static void mbedtls_sha256(const unsigned char *id,size_t n,unsigned char *out,int mode){
    assert(!mode&&n==47&&strlen((const char*)id)==n);
    static const unsigned char hash[32]={DIGEST};memcpy(out,hash,sizeof(hash));
}
""".replace("DIGEST", ",".join(str(x) for x in digest))
    for capacity in (32, 33, 41, 42):
        checks = []
        for domain in (0, 1, 0xffffffff):
            prefix = "as-" if not domain else f"ns-{domain:08x}-"
            for slot in (0, 1):
                legacy = f"/{prefix}{digest[:8].hex()}-{slot}"
                for epoch in (0, 1, 0xffffffff):
                    path = legacy if not epoch else f"{legacy}-e{epoch:08x}"
                    if len(path) >= capacity:
                        fingerprint = base64.urlsafe_b64encode(digest[:9]).decode()
                        path = f"/e{slot}{domain:08x}{epoch:08x}{fingerprint}"
                    checks.append(f"epoch_path_for(id,{domain}u,{slot},{epoch}u,out);"
                                  f"assert(!strcmp(out,{json.dumps('/spiffs' + path)}));")
        program = tmp_path / f"names-{capacity}.c"
        program.write_text(backend + paths + '\nint main(void){char out[64];const char *id='
                           + json.dumps(component) + ";\n" + "\n".join(checks) + "\nreturn 0;}\n")
        exe = program.with_suffix("")
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        f"-DCONFIG_SPIFFS_OBJ_NAME_LEN={capacity}", program, "-o", exe], check=True)
        subprocess.run([exe], check=True)
