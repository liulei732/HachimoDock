from pathlib import Path
import subprocess


def test_scoped_test_message(tmp_path):
    tests = Path(__file__).resolve().parent
    vendor = tests / "vendor/cjson"
    binary = tmp_path / "p4-test-message"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
        "-fsanitize=address,undefined", "-I", str(vendor), str(tests / "p4_test_message_test.c"),
        str(vendor / "cJSON.c"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
