"""Flash a complete P4 image only after checking the connected silicon revision.

Requires esptool 5.4.0 (including P4 v3.2 stub support). Keeps one serial connection from chip detection through
erase/write/verify; no hardware is touched until the caller explicitly runs it.
"""
import argparse
from contextlib import closing
import hashlib
import json
import re
import secrets
import time
from pathlib import Path

from p4_image_compat import factory_revision_range, image_application_version


def checked_image(image, manifest_path, board, pinned_sha256=None):
    meta = json.loads(manifest_path.read_text(encoding="utf-8"))
    if board not in ("pico", "wifi6") or meta.get("boardModel") != board:
        raise ValueError("实物板型与工厂清单不匹配，未擦除数据")
    if (meta.get("format") != "pet-manager-p4-factory-v1" or meta.get("completeInstall") is not True
            or meta.get("chip") != "esp32p4" or meta.get("flashOffset") != "0x0"
            or meta.get("image") != image.name):
        raise ValueError("工厂清单与所选完整镜像不匹配")
    if pinned_sha256 is not None and meta.get("sha256") != pinned_sha256:
        raise ValueError("工厂清单 SHA-256 与启动器记录不匹配")
    data = image.read_bytes()
    if len(data) != meta.get("bytes") or hashlib.sha256(data).hexdigest() != meta.get("sha256"):
        raise ValueError("完整镜像长度或 SHA-256 与工厂清单不匹配")
    version = image_application_version(data[0x10000:0x10000 + 112])
    if version != meta.get("version") or not version.endswith("-" + board):
        raise ValueError("镜像中的固件版本或板型与实物、清单不匹配")
    revisions = factory_revision_range(data)
    if revisions != (meta.get("chipRevisionMin"), meta.get("chipRevisionMax")):
        raise ValueError("镜像芯片范围与清单不匹配")
    return data, revisions


def application_identity(port):
    """Bounded read-only query before ROM entry; no bind, reset, or stored credentials."""
    import serial
    request_id = secrets.token_hex(8)
    query = (json.dumps({"topic": "debug/identity", "payload": {"requestId": request_id}}) + "\n").encode()
    observed_id = None
    for baud in (4000000, 2000000, 3000000, 921600, 115200):
        try:
            with closing(serial.Serial(port=None, baudrate=baud, timeout=.1, write_timeout=.3)) as stream:
                stream.dtr = False
                stream.rts = False
                stream.port = port
                stream.open()
                stream.write(query)
                end = time.monotonic() + .6
                while time.monotonic() < end:
                    line = stream.read_until(size=16384)
                    try:
                        event = json.loads(line)
                    except (ValueError, UnicodeError):
                        continue
                    if not isinstance(event, dict):
                        continue
                    payload = event.get("payload")
                    if not isinstance(payload, dict):
                        continue
                    board_id = payload.get("boardDeviceId", "")
                    if not isinstance(board_id, str) or not re.fullmatch(r"p4-[0-9a-f]{12}", board_id):
                        continue
                    if event.get("topic") == "debug/identity" and payload.get("requestId") == request_id:
                        observed_id = board_id
                        model = payload.get("hardwareBoardModel")
                        if model in ("pico", "wifi6"):
                            return board_id, model
                    if event.get("topic") == "hello":
                        caps = payload.get("capabilities")
                        info = caps.get("firmwareUpdate") if isinstance(caps, dict) else None
                        if isinstance(info, dict) and info.get("hardwareBoardModel") in ("pico", "wifi6"):
                            return board_id, info["hardwareBoardModel"]
        except (serial.SerialException, OSError, ValueError):
            continue
    return (observed_id, None) if observed_id else None


def flash(image: Path, port: str, baud: int = 921600, *, manifest: Path, board: str, device_id=None, pinned_sha256=None):
    import esptool
    from esptool.cmds import run_stub, write_flash

    if esptool.__version__ != "5.4.0":
        raise ValueError("请安装 esptool==5.4.0 后重试；旧版烧录工具不支持此 P4 芯片，未擦除数据")
    data, (minimum, maximum) = checked_image(image, manifest, board, pinned_sha256)
    observed = application_identity(port)
    if observed and observed[1] is not None and observed[1] != board:
        raise ValueError("设备报告的硬件板型与所选板型不同，未擦除数据")
    with esptool.detect_chip(port, baud=115200) as connected:
        if connected.CHIP_NAME != "ESP32-P4":
            raise ValueError("设备不是 ESP32-P4，已取消烧录，未擦除数据")
        revision = connected.get_chip_revision()
        if not minimum <= revision <= maximum:
            raise ValueError(f"芯片 {revision // 100}.{revision % 100} 与固件不匹配，已取消烧录，未擦除数据")
        if observed or device_id:
            rom_id = "p4-" + bytes(connected.read_mac()).hex()
            if (observed and rom_id != observed[0]) or (device_id and rom_id != device_id):
                raise ValueError("ROM 设备与已核对的实物设备不一致，未擦除数据")
        device = run_stub(connected)
        device.change_baud(baud)
        # esptool performs image checks, complete erase, write, and verification.
        write_flash(device, [(0, data)], erase_all=True, compress=True)
        device.hard_reset()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="验证芯片版本后烧录完整 P4 镜像（将清空设备）")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--board", choices=("pico", "wifi6"), required=True, help="操作员核对的实体板型")
    parser.add_argument("--sha256", help="打包时记录的完整镜像 SHA-256")
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=921600)
    args = parser.parse_args()
    flash(args.image, args.port, args.baud, manifest=args.manifest, board=args.board, pinned_sha256=args.sha256)
