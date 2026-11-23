"""Execute the shipping keyboard protocol and setters against cJSON and in-memory NVS."""
from pathlib import Path
import re
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"
CJSON = ROOT / "managed_components/espressif__cjson/cJSON"


@pytest.mark.parametrize("nimble", [0, 1])
def test_keyboard_condition_inside_dispatch(tmp_path, nimble):
    protocol = (MAIN / "pet_p4_protocol.c").read_text()
    dispatch = protocol[protocol.index("static bool keyboard_integer("):protocol.index("static void send_wifi_status(",
                        protocol.index("static bool keyboard_integer("))]
    ble = (MAIN / "ble_budget_probe.c").read_text()
    start = ble.rindex("typedef struct {", 0, ble.index("} keyboard_config_t;"))
    setters = ble[start:ble.index("static bool trusted_peer(", start)]
    code = (ROOT / "tests/p4_keyboard_conditional_host.c").read_text()
    source = tmp_path / "keyboard.c"
    source.write_text(code.replace("/* Production setters */", setters).replace("/* Production protocol */", dispatch))
    binary = tmp_path / "keyboard"
    subprocess.run(["cc", "-std=c11", "-fsanitize=address,undefined", "-Wall", "-Werror", "-Wno-deprecated-declarations",
                    "-DPET_P4_SYSTEM_WIFI=1", f"-DCONFIG_BT_NIMBLE_ENABLED={nimble}",
                    "-I", str(CJSON), str(source), str(CJSON / "cJSON.c"), "-o", str(binary)], check=True)
    subprocess.run([binary], check=True)


def test_all_configuration_writers_share_the_device_state_mutex():
    # Guard the premise of the conditional update if either transport or local
    # input is moved to another task. NimBLE's radio task only reads the layout.
    main = (MAIN / "pet_p4_main.c").read_text()
    for call in ("pet_p4_handle_line(&g_state,", "pet_p4_handle_line_with_reply(&g_state,",
                 "pet_p4_lan_control_run(&g_state,", "pet_p4_touch_process(&g_state,",
                 "pet_p4_input_process(&g_state,"):
        i = main.index(call)
        assert main.rfind("xSemaphoreTake(g_state_mutex", 0, i) > main.rfind("xSemaphoreGive(g_state_mutex", 0, i)
        assert main.index("xSemaphoreGive(g_state_mutex", i) < main.index("xSemaphoreTake(g_state_mutex", i)
    assert re.search(r"pet_p4_native_usb_init\(\s*&g_state,\s*g_state_mutex,\s*enqueue_protocol_line,", main)
    native = (MAIN / "pet_p4_usb_native.c").read_text()
    dispatch = native.index("handle_complete_frame(frame.kind")
    assert native.rfind("xSemaphoreTake(g_state_mutex", 0, dispatch) > native.rfind("xSemaphoreGive(g_state_mutex", 0, dispatch)
    assert "g_state_mutex = state_mutex;" in native
    lan = (MAIN / "pet_p4_lan_component.inc").read_text()
    branch = lan.split("delivery.operation == PET_LAN_CONTROL_KEYBOARD) {")[1].split("} else if")[0]
    assert "admitted && !atomic_load(&usb_client_active) && component_socket_live(NULL)" in branch
    assert "pet_p4_keyboard_config(delivery.keyboard_request" in branch
    protocol = (MAIN / "pet_p4_protocol.c").read_text()
    assert 'pet_p4_keyboard_config(payload, state->board_device_id)' in protocol.split('strcmp(topic, "keyboard/config")')[1].split("} else")[0]
    writers = {file.name for file in MAIN.iterdir() if file.suffix in {".c", ".inc"}
               and re.search(r"\bpet_ble_keyboard_set_(mode|chord)\s*\(", file.read_text())}
    assert writers == {"ble_budget_probe.c", "pet_p4_protocol.c", "pet_p4_system_settings.c"}
