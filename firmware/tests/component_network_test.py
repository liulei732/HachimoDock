from pathlib import Path
import subprocess
import os
import pytest

def test_component_network_core(tmp_path):
    root = Path(__file__).resolve().parents[1]
    exe = tmp_path / "component-network"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-Wno-misleading-indentation", "-Wno-deprecated-declarations", "-fsanitize=address,undefined", "-g",
        "-I" + str(root / "main"), "-I" + str(root / "tests/vendor/cjson"),
        str(root / "tests/component_network_test.c"),
        str(root / "main/pet_component_network.c"),
        str(root / "tests/vendor/cjson/cJSON.c"), "-pthread", "-o", str(exe)], check=True)
    subprocess.run([str(exe), str(root / "tests/component_network_policies.json")], check=True, timeout=15)

@pytest.mark.parametrize("real_vault", [False, True], ids=["transport", "aes-gcm"])
def test_component_network_device_backend(tmp_path, real_vault):
    root = Path(__file__).resolve().parents[1]
    exe = tmp_path / "component-network-device"
    crypto = []
    if real_vault:
        if not os.environ.get("PETUI_TEST_MBEDTLS_SOURCE"):
            pytest.skip("set PETUI_TEST_MBEDTLS_SOURCE for production AES-GCM")
        mbed = Path(os.environ["PETUI_TEST_MBEDTLS_SOURCE"])
        crypto = ["-DPET_TEST_REAL_VAULT", "-DPET_P4_COMPONENT_NETWORK=1", "-DPET_P4_SYSTEM_WIFI=1",
                  '-DMBEDTLS_CONFIG_FILE="network_mbedtls_config.h"', "-I" + str(mbed / "include"),
                  str(root / "main/pet_p4_private_feed.c"),
                  *(str(mbed / "library" / (n + ".c")) for n in
                    ("aes", "cipher", "cipher_wrap", "constant_time", "gcm", "sha256", "platform_util"))]
    subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200112L", "-Wall", "-Wextra", "-Werror",
        "-Wno-misleading-indentation", "-Wno-deprecated-declarations", "-Wno-sign-compare", "-fsanitize=address,undefined", "-g", *crypto,
        "-I" + str(root / "main"), "-I" + str(root / "tests/vendor/cjson"),
        "-I" + str(root / "tests/component_network_stubs"),
        str(root / "tests/component_network_device_test.c"),
        str(root / "main/pet_component_network.c"),
        str(root / "tests/vendor/cjson/cJSON.c"), "-pthread", "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=15)
