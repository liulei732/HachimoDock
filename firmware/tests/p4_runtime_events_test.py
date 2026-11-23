from pathlib import Path
import subprocess


def test_runtime_events_real_dispatch_and_handlers(tmp_path):
    firmware = Path(__file__).resolve().parents[1]
    cjson = firmware / "tests/vendor/cjson"
    binary = tmp_path / "runtime-events"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-error=deprecated-declarations",
                    "-fsanitize=address,undefined", "-I", firmware / "main",
                    "-I", cjson, firmware / "tests/p4_runtime_events_test.c",
                    cjson / "cJSON.c", "-o", binary], check=True)
    subprocess.run([binary], check=True, timeout=15)
