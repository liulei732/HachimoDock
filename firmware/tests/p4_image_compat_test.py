"""Host-only revision checks; no serial hardware or cloud access."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys
import types

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from p4_image_compat import image_revision_range, factory_revision_range
from flash_checked import flash


def image(minimum, maximum):
    data = bytearray(24)
    data[0] = 0xe9
    data[12] = 18
    data[15:17] = minimum.to_bytes(2, "little")
    data[17:19] = maximum.to_bytes(2, "little")
    return data


def factory(minimum, maximum):
    data = bytearray(0x10000 + 112)
    data[0x10020:0x10024] = bytes.fromhex("3254cdab")
    label = b"test-wifi6"
    data[0x10030:0x10030+len(label)] = label
    for offset in [0x2000, 0x10000]:
        data[offset:offset + 24] = image(minimum, maximum)
    return data


def manifest_for(image, board="wifi6"):
    data = image.read_bytes()
    meta = dict(version="test-wifi6",format="pet-manager-p4-factory-v1", completeInstall=True, chip="esp32p4",
                flashOffset="0x0", image=image.name, boardModel=board, bytes=len(data),
                sha256=hashlib.sha256(data).hexdigest(), chipRevisionMin=300, chipRevisionMax=399)
    path = image.with_suffix(".json")
    path.write_text(json.dumps(meta))
    return path


def test_device_header_matrix(tmp_path):
    binary = tmp_path / "chip-test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "main"), str(ROOT / "tests/p4_image_compat_test.c"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_v3_build_is_separate_without_changing_transport_or_ui():
    platform = (ROOT / "platformio.ini").read_text()
    v3 = platform.split("[env:esp32_p4_evboard_v3]", 1)[1]
    assert "55.03.38-1" in v3
    assert "board_build.f_cpu = 400000000L" in v3
    assert "custom_p4_uart_baud" not in v3
    assert "monitor_speed" not in v3
    assert "custom_p4_uart_baud = 4000000" in platform
    overlay = (ROOT / "sdkconfig.v3.defaults").read_text()
    assert "CONFIG_ESP32P4_REV_MIN_300=y" in overlay
    assert "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_400=y" in overlay
    assert "CONFIG_ESP32P4_SELECTS_REV_LESS_V3 is not set" in overlay
    cmake = (ROOT / "CMakeLists.txt").read_text()
    assert "dependencies.v3.lock" in cmake
    for name, version in [("dependencies.lock", "5.5.1"), ("dependencies.v3.lock", "5.5.4")]:
        assert "version: " + version in (ROOT / name).read_text()


def test_private_link_reference_never_overrides_normal_ui_or_windows():
    import configparser
    config=configparser.ConfigParser(interpolation=None)
    config.read(ROOT/'platformio.ini')
    assert config['env:esp32_p4_uart_2m_reference']['custom_p4_uart_baud']=='2000000'
    assert config['env:esp32_p4_uart_2m_reference']['extends']=='env:esp32_p4_canvas_candidate'
    assert config['env:esp32_p4_evboard']['custom_p4_uart_baud']=='4000000'
    for name in ('esp32_p4_canvas_candidate','esp32_p4_evboard_windows','esp32_p4_evboard_v3'):
        assert 'custom_p4_uart_baud' not in config['env:'+name]


def test_factory_rejects_unbounded_and_mixed_images():
    for lo, hi in [(1,199), (300,399)]:
        assert factory_revision_range(factory(lo,hi)) == (lo,hi)
    for lo, hi in [(0,65535), (1,399), (300,199), (200,299)]:
        with pytest.raises(ValueError):
            image_revision_range(image(lo,hi))
    mixed = factory(1,199)
    mixed[0x10000:0x10018] = image(300,399)
    with pytest.raises(ValueError):
        factory_revision_range(mixed)


@pytest.mark.parametrize("revision", [1,100,199,200,300,301,302,399,400])
def test_flash_checks_revision_before_any_erase_or_write(tmp_path, monkeypatch, revision):
    events = []
    class Device:
        CHIP_NAME = "ESP32-P4"
        def __enter__(self): return self
        def __exit__(self, *_): events.append("closed")
        def get_chip_revision(self): events.append("revision"); return revision
        def change_baud(self, baud): events.append("baud")
        def hard_reset(self): events.append("reset")
    device = Device()
    monkeypatch.setitem(sys.modules, "esptool", types.SimpleNamespace(__version__="5.4.0", detect_chip=lambda *a,**k: device))
    def write(esp, data, **kwargs):
        assert esp is device and kwargs == {"erase_all": True, "compress": True}
        events.append("write")
    monkeypatch.setitem(sys.modules, "esptool.cmds", types.SimpleNamespace(run_stub=lambda d:d, write_flash=write))
    path = tmp_path / "factory.bin"
    path.write_bytes(factory(300,399))
    manifest = manifest_for(path)
    monkeypatch.setattr("flash_checked.application_identity", lambda port: None)
    if 300 <= revision <= 399:
        flash(path, "test-port", manifest=manifest, board="wifi6")
        assert events == ["revision", "baud", "write", "reset", "closed"]
    else:
        with pytest.raises(ValueError): flash(path, "test-port", manifest=manifest, board="wifi6")
        assert events == ["revision", "closed"]


def test_flash_rejects_old_tool_before_opening_device(tmp_path, monkeypatch):
    def unexpected(*args, **kwargs):
        pytest.fail("old tool must not open the device or write flash")
    monkeypatch.setitem(sys.modules, "esptool", types.SimpleNamespace(__version__="5.2.0", detect_chip=unexpected))
    monkeypatch.setitem(sys.modules, "esptool.cmds", types.SimpleNamespace(run_stub=unexpected, write_flash=unexpected))
    with pytest.raises(ValueError, match="5.4.0"):
        flash(tmp_path / "not-read.bin", "test-port", manifest=tmp_path/"unused", board="pico")


def test_packaged_tools_upgrade_existing_environments():
    source = (ROOT.parent / "pc/scripts/package-factory-release.mjs").read_text()
    assert source.count('esptool==5.4.0') == 2
    assert source.count('__import__("esptool").__version__ == "5.4.0"') == 2
    assert 'esptool==5.2.0' not in source

@pytest.mark.parametrize("failure", ["digest", "length", "physical_board", "unknown_board", "observed_board", "rom_device", "image_board", "pin"])
def test_flashing_stops_before_stub_or_erase(tmp_path, monkeypatch, failure):
    image = tmp_path / "factory.bin"
    data = factory(300,399)
    image.write_bytes(data)
    manifest = manifest_for(image)
    board = {"physical_board": "pico", "unknown_board": ""}.get(failure, "wifi6")
    if failure == "digest":
        data[-1] ^= 1
        image.write_bytes(data)
    if failure == "length":
        image.write_bytes(data[:-1])
    if failure == "image_board":
        board = "pico"
        manifest = manifest_for(image, board=board)
    class Rom:
        CHIP_NAME = "ESP32-P4"
        def __enter__(self): return self
        def __exit__(self, *args): pass
        def get_chip_revision(self): return 302
        def read_mac(self): return bytes(6)
    def detect(*args, **kwargs):
        assert failure == "rom_device"
        return Rom()
    monkeypatch.setitem(sys.modules, "esptool", types.SimpleNamespace(__version__="5.4.0", detect_chip=detect))
    def forbidden(*args, **kwargs): pytest.fail("must stop before stub/erase/write")
    monkeypatch.setitem(sys.modules, "esptool.cmds", types.SimpleNamespace(run_stub=forbidden, write_flash=forbidden))
    monkeypatch.setattr("flash_checked.application_identity", lambda port:
                        ("p4-111111111111", "pico" if failure == "observed_board" else "wifi6"))
    with pytest.raises(ValueError):
        flash(image, "test-port", manifest=manifest, board=board, pinned_sha256="00"*32 if failure=="pin" else None)


def test_factory_app_probe_reads_only_hardware_identity(monkeypatch):
    from flash_checked import application_identity
    events = []
    class Serial:
        def __init__(self, **kwargs):
            assert kwargs["port"] is None
            self.responses = []
        def open(self):
            assert self.dtr is False and self.rts is False
        def write(self, request):
            query = json.loads(request)
            assert query["topic"] == "debug/identity"
            request_id = query["payload"]["requestId"]
            common = dict(boardDeviceId="p4-111111111111")
            self.responses = [
                {"topic":"debug/identity","payload":dict(common,requestId="stale",hardwareBoardModel="pico")},
                {"topic":"hello","payload":dict(common,capabilities={"firmwareUpdate":{"boardModel":"pico"}})},
                {"topic":"debug/identity","payload":dict(common,requestId=request_id,hardwareBoardModel="wifi6")},
            ]
        def read_until(self, size):
            assert size == 16384
            return json.dumps(self.responses.pop(0)).encode()
        def close(self): events.append("close")
    monkeypatch.setitem(sys.modules, "serial", types.SimpleNamespace(Serial=Serial, SerialException=OSError))
    assert application_identity("fixture") == ("p4-111111111111", "wifi6")
    assert events == ["close"]


def test_generated_launchers_pass_operator_model_and_pinned_digest():
    source = (ROOT.parent / "pc/scripts/package-factory-release.mjs").read_text()
    script = source[source.index("const adapterBeforeFlash"):source.index("function windowsFlashScript")]
    windows = source[source.index("function windowsFlashScript"):]
    script += windows[:windows.index("`;\n}") + 4]
    script += """
    const assert = require("node:assert/strict");
    const mac = macFlashScript("image.bin","manifest.json","wifi6","abcd");
    const win = windowsFlashScript("image.bin","manifest.json","wifi6","abcd");
    for (const output of [mac,win]) {
      assert.ok(output.includes("--manifest") && output.includes("manifest.json") && output.includes('--sha256 "abcd"'));
      assert.ok(output.includes("输入实际板型") && output.includes("ERASE"));
    }
    assert.ok(mac.includes('--board "${physical_board}"'));
    assert.ok(win.includes('--board $PhysicalBoard'));
    """
    subprocess.run(["node", "-e", script], check=True)


def test_factory_packager_validates_embedded_application_before_export():
    source = (ROOT.parent / "pc/scripts/package-factory-release.mjs").read_text()
    check = source[source.index("function checkEmbeddedVersion("):source.index("// Shown before entering")]
    assert source.index("checkEmbeddedVersion(factoryBytes, manifest.version)") < source.index("mkdirSync(segmentDir")
    subprocess.run(["node", "-e", check + """
        const assert = require("node:assert/strict");
        const bytes = Buffer.alloc(0x10070);
        bytes.writeUInt32LE(0xabcd5432, 0x10020);
        bytes.write("test-wifi6", 0x10030);
        checkEmbeddedVersion(bytes, "test-wifi6");
        assert.throws(() => checkEmbeddedVersion(bytes, "test-pico"));
        bytes[0x10020] = 0;
        assert.throws(() => checkEmbeddedVersion(bytes, "test-wifi6"));
        assert.throws(() => checkEmbeddedVersion(Buffer.alloc(20), "test-wifi6"));
    """], check=True)
