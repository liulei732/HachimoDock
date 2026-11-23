from pathlib import Path
import subprocess


def test_paired_component_network_dispatch(tmp_path):
    firmware = Path(__file__).resolve().parents[1]
    vendor = firmware / "tests/vendor/cjson"
    exe = tmp_path / "paired-network"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations", "-fsanitize=address,undefined",
                    "-I", str(vendor), str(firmware / "tests/component_network_lan_test.c"),
                    str(vendor / "cJSON.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)
