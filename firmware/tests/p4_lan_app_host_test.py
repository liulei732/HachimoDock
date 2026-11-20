"""Exercise the real authenticated-LAN mailbox after frame authentication."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_component_event_ack_snapshot_and_route_retirement(tmp_path):
    vendor = ROOT / "tests/vendor/cjson"
    binary = tmp_path / "lan-app-host"
    subprocess.run([
        "clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-Wno-deprecated-declarations", "-I", str(ROOT / "main"), "-I", str(vendor),
        str(ROOT / "tests/p4_lan_app_host_test.c"), str(vendor / "cJSON.c"), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
