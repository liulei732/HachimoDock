"""Encode one launcher tile as a 16-color RGB565 palette and 64×48 four-bit pixels."""
import argparse
import io
import struct
import sys
from PIL import Image


def thumbnail(image):
    reduced = image.quantize(colors=16, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    palette = (reduced.getpalette() + [0] * 48)[:48]
    result = bytearray()
    for r, g, b in zip(palette[::3], palette[1::3], palette[2::3]):
        result.extend(struct.pack("<H", (r >> 3) << 11 | (g >> 2) << 5 | b >> 3))
    indices = reduced.tobytes()
    result.extend((a << 4) | b for a, b in zip(indices[::2], indices[1::2]))
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cover", action="store_true", help="read a package PNG cover")
    args = parser.parse_args()
    if args.cover:
        with Image.open(io.BytesIO(sys.stdin.buffer.read())) as cover:
            if cover.format != "PNG" or cover.size not in {(320, 240), (640, 480)}:
                parser.error("expected a 320×240 or 640×480 PNG cover")
            flattened = Image.new("RGBA", cover.size, "white")
            flattened.alpha_composite(cover.convert("RGBA"))
            image = flattened.convert("RGB").resize((64, 48), Image.Resampling.LANCZOS)
    else:
        image = Image.frombytes("RGB", (64, 48), sys.stdin.buffer.read())
    sys.stdout.buffer.write(thumbnail(image))
