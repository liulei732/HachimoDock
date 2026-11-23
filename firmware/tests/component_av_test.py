"""Exercise the production mode 25 parser and note queue with host peripherals."""
from pathlib import Path
import shutil
import subprocess


def test_component_av(tmp_path):
    fw = Path(__file__).resolve().parents[1]
    vendor = fw / "tests/vendor/cjson"
    for name in ("pet_p4_lan_component_av.inc", "pet_p4_component_av.h",
                 "pet_p4_note_frames.inc", "pet_p4_json_source.h"):
        shutil.copy(fw / "main" / name, tmp_path / name)
    for name in ("pet_p4_audio.h", "pet_p4_app_host.h"):
        (tmp_path / name).write_text("")
    (tmp_path / "esp_err.h").write_text("typedef int esp_err_t;\n#define ESP_OK 0\n")
    exe = tmp_path / "av"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-deprecated-declarations", "-fsanitize=address,undefined", "-I", str(tmp_path), "-I", str(vendor),
                    str(fw / "tests/component_av_test.c"), str(vendor / "cJSON.c"),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
