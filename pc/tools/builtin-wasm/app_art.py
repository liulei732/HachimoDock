"""Bake antialiased WASM UI layers to RGB565 palette sprites."""
from pathlib import Path
import os
import struct

from PIL import Image, ImageDraw, ImageFont

S = 3


def font(size, bold=False, cjk=False):
    fonts = Path(os.environ["HACHI_ART_FONTS"])
    name = "NotoSansSC-Regular.ttf" if cjk else "Montserrat-Bold.ttf" if bold else "Montserrat-Medium.ttf"
    path = fonts / name
    return ImageFont.truetype(str(path), size * S)


def scale_box(rect):
    return tuple(round(v * S) for v in rect)


def line(im, points, fill, width=1):
    ImageDraw.Draw(im).line(scale_box(points), fill, width * S)


def text(im, pos, value, size, color, *, bold=False, center=False):
    ImageDraw.Draw(im).text(scale_box(pos), value, color, font(size, bold, not value.isascii()),
                            anchor="mm" if center else "lt")


def rgb(hex_color):
    return tuple(bytes.fromhex(hex_color.lstrip("#")))


def gradient(im, rect, radius, top, bottom, outline=None):
    x, y, w, h = rect
    extent = (w * S, h * S)
    tile = Image.new("RGBA", extent)
    draw = ImageDraw.Draw(tile)
    a, b = rgb(top), rgb(bottom)
    for row in range(extent[1]):
        blend = row / max(1, extent[1] - 1)
        fill = tuple(round(l + (r - l) * blend) for l, r in zip(a, b))
        draw.line((0, row, extent[0], row), fill)
    mask = Image.new("L", extent)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, extent[0]-1, extent[1]-1), radius * S, 255)
    tile.putalpha(mask)
    if outline:
        ImageDraw.Draw(tile).rounded_rectangle((0, 0, extent[0]-1, extent[1]-1), radius * S, outline=outline, width=S)
    im.alpha_composite(tile, (x*S, y*S))


def canvas(w, h):
    return Image.new("RGBA", (w * S, h * S))


def artwork_piano():
    art = {}
    im = canvas(320, 240)
    gradient(im, (0, 0, 320, 240), 0, "#392725", "#171419")
    gradient(im, (8, 8, 304, 74), 13, "#49362f", "#2f2528", "#695146")
    text(im, (20, 17), "模拟琴键", 23, "#fff0dc")
    text(im, (21, 52), "Nocturne  ·  C Major", 10, "#d9b9a0")
    for i in range(4):
        line(im, (150, 21+i*7, 207, 21+i*7), "#70534a")
    gradient(im, (222, 14, 80, 59), 10, "#644936", "#40302d", "#80614b")
    text(im, (262, 25), "音区", 10, "#ecd4bc", center=True)
    gradient(im, (8, 87, 304, 138), 13, "#22191e", "#0d0d13", "#543c38")
    line(im, (16, 93, 303, 93), "#8b6050")
    text(im, (160, 232), "触屏弹奏 · 摇杆选音 · 1 弹奏 · 2 切换音区", 10, "#e2c7b4", center=True)
    notes = ("C", "D", "E", "F", "G", "A", "B", "C")
    colors = ("#a394e7", "#89a7e5", "#81bbdf", "#7bc9bc", "#9fcba0", "#d5b486", "#dc9c87", "#d69ab0")
    for i, (note, accent) in enumerate(zip(notes, colors)):
        for pressed in (False, True):
            key = canvas(34, 116)
            gradient(key, (0, 0, 34, 115), 6, "#120e16", "#120e16")
            lift = 4 if pressed else 0
            gradient(key, (1, lift, 32, 110), 5,
                     "#fbecd8" if not pressed else "#fff5e8",
                     "#c4aaa0" if not pressed else accent)
            line(key, (6, lift+3, 28, lift+3), "#fff8e8")
            line(key, (7, lift+76, 26, lift+76), accent, 2)
            text(key, (17, lift+91), note, 18, "#473330", bold=True, center=True)
            art[f"key_{i}_{int(pressed)}"] = key
    divider = canvas(12, 33)
    gradient(divider, (0, 0, 12, 33), 4, "#4a383a", "#20191f", "#241a1c")
    art["divider"] = divider
    art["backdrop"] = im
    for name, value in [("low", "低八度"), ("high", "高八度")]:
        badge = canvas(72, 27)
        text(badge, (36, 13), value, 15, "#ffecd4", center=True)
        art[name] = badge
    return art


def packbits(data):
    result = bytearray()
    cursor = 0
    while cursor < len(data):
        end = cursor + 1
        while end < len(data) and end - cursor < 128 and data[end] == data[cursor]:
            end += 1
        if end - cursor >= 3:
            result += bytes((127 + end - cursor, data[cursor]))
        else:
            while end < len(data) and end - cursor < 128 and not (
                end + 2 < len(data) and data[end] == data[end+1] == data[end+2]
            ):
                end += 1
            result.append(end - cursor - 1)
            result += data[cursor:end]
        cursor = end
    return result


def encode(im, colors=80):
    # Composite edge pixels against a near-black frame before palette reduction.
    # Keep the interior antialiasing and skip the fully transparent exterior.
    im = im.resize((im.width // S, im.height // S), Image.Resampling.LANCZOS)
    base = Image.new("RGB", im.size, "#241d21")
    alpha = im.getchannel("A")
    base.paste(im, mask=alpha)
    indexed = base.quantize(colors=colors, dither=Image.Dither.NONE)
    palette = indexed.getpalette()
    size = max(indexed.getdata()) + 1
    data = bytes(v + 1 if a >= 36 else 0 for v, a in zip(indexed.getdata(), alpha.getdata()))
    encoded = bytearray([size])
    for i in range(size):
        r, g, b = palette[3*i:3*i+3]
        encoded += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    encoded += packbits(bytes(0 if i >= im.width and value == data[i-im.width] else value + 1
                              for i, value in enumerate(data)))
    return im.width, im.height, encoded
