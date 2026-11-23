from pathlib import Path
import subprocess


def test_hid_queue_delivery_boundaries(tmp_path):
    root = Path(__file__).resolve().parents[1]
    executable = tmp_path / "hid-queue"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I", str(root / "main"), str(root / "tests/p4_hid_queue_test.c"),
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
