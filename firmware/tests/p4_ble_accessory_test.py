"""Exercise the production accessory broker with the radio/time boundaries replaced."""
import json
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
SKILL = ROOT / "pc/skills/petui"
sys.path.insert(0, str(SKILL / "scripts"))
from validate_generated_widget import DEFAULT_CAPABILITIES, validate_widget
from smoke_test_widget_game import run_smoke_test


def test_foreground_radio_broker(tmp_path):
    for header in ("esp_timer.h", "freertos/FreeRTOS.h", "host/ble_hs.h", "nimble/hci_common.h"):
        stub = tmp_path / header
        stub.parent.mkdir(parents=True, exist_ok=True)
        stub.touch()
    exe = tmp_path / "accessory"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-fsanitize=address,undefined",
                    "-DPET_P4_SYSTEM_WIFI=1", "-DCONFIG_BT_NIMBLE_ENABLED=1",
                    "-DCONFIG_BT_NIMBLE_ROLE_CENTRAL=1", "-DCONFIG_BT_NIMBLE_GATT_CLIENT=1",
                    "-I", str(tmp_path), "-I", str(ROOT / "firmware/main"),
                    str(Path(__file__).with_name("p4_ble_accessory_host.c")), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


@pytest.mark.parametrize("name", ["ble-accessory-battery", "ble-room-temperature"])
def test_accessory_examples_and_legacy_admission(name):
    path = SKILL / "examples" / name
    assert not validate_widget(path)
    report = run_smoke_test(path)
    assert report["ok"] and report["mode"] == "foreground-ble-read" and not report["deviceVerified"]
    legacy = dict(DEFAULT_CAPABILITIES)
    del legacy["widgetBleAccessory"]
    assert any("widgetBleAccessory" in e for e in validate_widget(path, legacy))


@pytest.mark.parametrize("extra", [{"accessory": "gatt"}, {"host": True}, {"peer": "supplied"},
                                    {"persist": {}}, {"hardware": {"service": "microphone-level-v1"}},
                                    {"accessory": {"uuid": "180f"}}, {"tick": [{}]}])
def test_declared_authority_is_bounded(tmp_path, extra):
    package = tmp_path / "example"
    shutil.copytree(SKILL / "examples/ble-accessory-battery", package)
    widget = package / "runtime/widget.json"
    value = json.loads(widget.read_text())
    value.update(extra)
    widget.write_text(json.dumps(value))
    assert any("accessory" in e for e in validate_widget(package))
