"""Exercise JPEG source selection with the pinned SDK and a newer revision."""

import os
from pathlib import Path
import shutil
import subprocess

import pytest


FIRMWARE = Path(__file__).resolve().parents[1]
BACKPORT = FIRMWARE / "overrides" / "esp-idf-v1-jpeg"
SDK = (
    Path(os.environ["PET_V1_SDK_ROOT"])
    if "PET_V1_SDK_ROOT" in os.environ
    else FIRMWARE.parents[1] / "toolchains/platformio/v1/packages/framework-espidf"
)


def configure(tmp_path, sdk_component, revision):
    cmake = shutil.which("cmake")
    if cmake is None:
        pytest.skip("cmake is unavailable")
    (tmp_path / "CMakeLists.txt").write_text(
        f'''cmake_minimum_required(VERSION 3.16)
project(jpeg_source_selection C)
set(IDF_TARGET esp32p4)
set(CONFIG_SOC_JPEG_DECODE_SUPPORTED 1)
set(CONFIG_ESP_REV_MAX_FULL {revision})
set(sdk_component "{sdk_component.as_posix()}")
add_library(jpeg STATIC "${{sdk_component}}/jpeg_decode.c"
  "${{sdk_component}}/jpeg_parse_marker.c" "${{sdk_component}}/jpeg_encode.c")
function(idf_component_get_property output component property)
  if(property STREQUAL "COMPONENT_LIB")
    set("${{output}}" jpeg PARENT_SCOPE)
  elseif(property STREQUAL "COMPONENT_DIR")
    set("${{output}}" "${{sdk_component}}" PARENT_SCOPE)
  endif()
endfunction()
include("{FIRMWARE.as_posix()}/tools/jpeg_v1_security.cmake")
get_target_property(jpeg_sources jpeg SOURCES)
file(WRITE "${{CMAKE_BINARY_DIR}}/jpeg_sources.txt" "${{jpeg_sources}}")
'''
    )
    return subprocess.run(
        [cmake, "-S", str(tmp_path), "-B", str(tmp_path / "build")],
        text=True, capture_output=True, check=False,
    )


def test_v3_uses_sdk_sources(tmp_path):
    vendor = tmp_path / "v3-sdk"
    vendor.mkdir()
    sources = [vendor / name for name in ("jpeg_decode.c", "jpeg_parse_marker.c", "jpeg_encode.c")]
    for source in sources:
        source.touch()
    result = configure(tmp_path, vendor, 399)
    assert result.returncode == 0, result.stderr
    assert (tmp_path / "build/jpeg_sources.txt").read_text().split(";") == [
        path.as_posix() for path in sources
    ]
    assert not (tmp_path / "build/pet_p4_jpeg_v1").exists()


def test_v1_replaces_only_pinned_decoder_sources(tmp_path):
    vendor = SDK / "components/esp_driver_jpeg"
    if not vendor.is_dir():
        pytest.skip("set PET_V1_SDK_ROOT to the pinned ESP-IDF v5.5.1 package")
    files = (
        "jpeg_decode.c", "jpeg_parse_marker.c", "jpeg_private.h", "jpeg_encode.c",
        "private/jpeg_param.h", "private/jpeg_parse_marker.h",
    )
    original = {name: (vendor / name).read_bytes() for name in files}
    result = configure(tmp_path, vendor, 199)
    assert result.returncode == 0, result.stderr
    generated = tmp_path / "build/pet_p4_jpeg_v1"
    assert (tmp_path / "build/jpeg_sources.txt").read_text().split(";") == [
        (generated / "jpeg_decode.c").as_posix(),
        (generated / "jpeg_parse_marker.c").as_posix(),
        (vendor / "jpeg_encode.c").as_posix(),
    ]
    for name in ("jpeg_decode.c", "jpeg_parse_marker.c", "jpeg_private.h"):
        assert (generated / name).read_bytes() == (BACKPORT / name).read_bytes()
    for name in ("private/jpeg_param.h", "private/jpeg_parse_marker.h"):
        assert (generated / name).read_bytes() == original[name]
    assert original == {name: (vendor / name).read_bytes() for name in files}

    unreviewed = tmp_path / "unreviewed-sdk"
    for name in files:
        path = unreviewed / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(original[name])
    (unreviewed / "jpeg_decode.c").write_bytes(original["jpeg_decode.c"] + b"\n")
    result = configure(tmp_path, unreviewed, 199)
    assert result.returncode != 0
    assert "unrecognized SDK source" in result.stderr
