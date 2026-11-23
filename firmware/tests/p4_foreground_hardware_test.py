"""Admission of two real source components, including the old-board and untrusted-package cases."""
import json
from pathlib import Path
import shutil
import sys

import pytest

SKILL = Path(__file__).resolve().parents[2] / "pc/skills/petui"
sys.path.insert(0, str(SKILL / "scripts"))
from validate_generated_widget import validate_widget, DEFAULT_CAPABILITIES
from smoke_test_widget_game import run_smoke_test


@pytest.mark.parametrize(("name", "threshold"), [("mic-level-meter", None), ("quiet-room-check", 20)])
def test_real_meter_examples_negotiate_hardware_separately_from_voice(name, threshold):
    package = SKILL / "examples" / name
    result = run_smoke_test(package)
    assert result["ok"] and result["mode"] == "foreground-hardware-contract"
    assert result["quietBelow"] == threshold and result["deviceVerified"] is False
    old = dict(DEFAULT_CAPABILITIES, audio=True, voice=True)
    del old["widgetMicLevel"]
    assert any("widgetMicLevel" in error for error in validate_widget(package, old))


@pytest.mark.parametrize("extra", [
    {"quiet_below": 0}, {"quiet_below": 101}, {"quiet_below": True}, {"quiet_below": "20"},
    {"service": "raw-pcm"}, {"grant": True}, {"remote": True}, {"background": True}, {"file": "samples"},
])
def test_hardware_contract_rejects_unimplemented_or_package_supplied_authority(tmp_path, extra):
    package = tmp_path / "example"
    shutil.copytree(SKILL / "examples/mic-level-meter", package)
    widget = package / "runtime/widget.json"
    runtime = json.loads(widget.read_text())
    runtime["hardware"].update(extra)
    widget.write_text(json.dumps(runtime))
    assert any("hardware" in error for error in validate_widget(package))


def test_only_physical_input_calls_meter_authorization():
    main = Path(__file__).resolve().parents[1] / "main"
    callers = {p.name for p in main.glob("*") if p.suffix in (".inc", ".c")
               and "pet_p4_miniapp_hardware_key(" in p.read_text()}
    assert callers == {"pet_p4_input.c", "pet_p4_miniapp.c"}
    code = (main / "pet_p4_input.c").read_text()
    physical = code[code.index("while (xQueueReceive(g_event_queue, &event"):]
    assert physical.index("handle_approval_input(") < physical.index("pet_p4_miniapp_hardware_key(")
    worker = (main / "pet_p4_audio.c").read_text().split("static void meter_work(void) {", 1)[1].split("static void audio_task", 1)[0]
    assert "pet_mic_peak(samples" in worker and "wipe[i] = 0" in worker
    assert not any(op in worker for op in ("send_", "fopen(", "fwrite(", "tap(", "esp_log", "ESP_LOG"))
