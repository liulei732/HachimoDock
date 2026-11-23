"""Run the device delete sweep over the cached IDF SPIFFS and RAM-backed NOR."""
import subprocess

import pytest

from p4_catalog_zero_rewrite_test import FIXTURES, ROOT, sdk_source, writer_hosts
from p4_render_trace_test import function


@pytest.mark.parametrize("speech", (0, 1))
def test_media_delete_scans_and_cold_cleanup_on_sdk_nor(writer_hosts, speech):
    work, _, env = writer_hosts
    sdk = sdk_source()
    vfs = (sdk.parents[1] / "esp_spiffs.c").read_text()
    media = (ROOT / "main/pet_p4_component_media.c").read_text()
    nor = (FIXTURES / "catalog_zero_rewrite_host.c.in").read_text().split('#include "cJSON.h"')[0]
    template = (FIXTURES / "media_delete_spiffs.c.in").read_text()
    cached_vfs = "\n".join(function(vfs, name) for name in (
        "spiffs_res_to_errno", "vfs_spiffs_opendir", "vfs_spiffs_closedir",
        "vfs_spiffs_readdir_r", "vfs_spiffs_readdir", "vfs_spiffs_unlink"))
    sweep = "\n".join(function(media, name) for name in (
        "kind_char", "media_path", *(("speech_path",) if speech else ()), "media_name_shape", "budget_scan_current",
        "media_remove_bit", "media_remove_snapshot", "unlink_media_candidate", "pet_cmedia_remove"))
    host = work / f"media-delete-{speech}.c"
    host.write_text(nor + template.replace("/* SDK_VFS */", cached_vfs).replace("/* DEVICE_SWEEP */", sweep))
    objects = [work / (name + ".c.o") for name in (
        "spiffs_cache", "spiffs_check", "spiffs_gc", "spiffs_hydrogen", "spiffs_nucleus")]
    binary = work / f"media-delete-{speech}"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    f"-DPET_P4_WASM_AUDIO={speech}", "-I", work / "include", "-I", sdk, "-I", ROOT / "main", "-I", ROOT / "tests/vendor/cjson",
                    host, *objects, "-o", binary], env=env, check=True)
    subprocess.run([binary], env=env, check=True, timeout=60)
