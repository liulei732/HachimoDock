"""Regenerate one built-in WASM using the repository SDK. Fonts are from LVGL."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
PC = ROOT.parents[1]
sys.path.insert(0, str(PC / "skills/petui/scripts"))
from compile_wasm import compile_wasm


def artwork(component, header):
    from app_art import artwork_piano, encode
    if component == "xiangqi":
        sys.path.insert(0, str(ROOT / component))
        from xiangqi_art import make_art
        sprites = make_art()
    else:
        sprites = artwork_piano()
    with header.open("w") as out:
        for name, image in sprites.items():
            w, h, data = encode(image, colors=32 if component == "xiangqi" else 80)
            print(f"#define {name}_w {w}\n#define {name}_h {h}", file=out)
            print(f"static const uint8_t art_{name}[] = {{", file=out)
            for i in range(0, len(data), 24):
                print(",".join(map(str, data[i:i+24])) + ",", file=out)
            print("};", file=out)


def rebuild(component):
    folder = ROOT / component
    runtime = PC / "builtin-clawpkgs" / component / "runtime"
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        for header in folder.glob("*.h"):
            shutil.copyfile(header, work / header.name)
        code = Path(shutil.copy(next(folder.glob("*.c")), work))
        if component != "wasm-2046":
            artwork(component, work / "app_art.h")
        module = work / "app.wasm"
        compile_wasm(code, PC / "skills/petui/sdk", os.environ.get("CLANG", "clang"),
                     os.environ.get("WASM_LD", "wasm-ld"), module)
        data = module.read_bytes()
        if len(data) > 65536:
            raise ValueError("WASM exceeds the 64 KiB profile")
        meta = runtime / "widget.json"
        widget = json.loads(meta.read_bytes())
        widget["hachiWasm"] = hashlib.sha256(data).hexdigest()
        (runtime / "app.wasm").write_bytes(data)
        meta.write_text(json.dumps(widget, ensure_ascii=False, separators=(",", ":")) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("component", choices=["wasm-piano", "wasm-2046", "xiangqi"])
    rebuild(parser.parse_args().component)
