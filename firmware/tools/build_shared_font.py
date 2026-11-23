"""Generate the 32px A2 bubble resource with FreeType 2.15, little-endian host."""
import argparse
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]

def generate(font, freetype_prefix=None):
    if sys.byteorder != 'little':
        raise ValueError('The rasterizer input stream requires a little-endian host')
    source = (ROOT/'main/pet_p4_font_cn.c').read_text()
    codes = [int(c,16) for c in re.findall(r'/\* U\+([0-9A-Fa-f]+) ',source)]
    advances = [int(a) for a in re.findall(r'\.bitmap_index = \d+, \.adv_w = (\d+)',source)][1:]
    assert len(codes)==len(advances) and codes==sorted(set(codes))
    han = {ord(c) for lead in range(0xb0,0xf8) for trail in range(0xa1,0xff)
           for c in bytes([lead,trail]).decode('gb2312','ignore')}
    entries = [(c,a) for c,a in zip(codes,advances) if c in han or 32<=c<=126 or
               0x3000<=c<0x3040 or 0xff01<=c<=0xff60 or chr(c) in '，。！：；（）“”‘’、…—·《》￥']
    assert len(entries)==6988
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp)/'raster'
        if freetype_prefix:
            flags = ['-I'+str(freetype_prefix/'include/freetype2'), '-L'+str(freetype_prefix/'lib'), '-lfreetype']
        else:
            flags = shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','freetype2'],text=True))
        subprocess.run(['cc','-std=c11',str(ROOT/'tools/render_shared_font.c'),*flags,'-o',exe],check=True)
        stream = subprocess.check_output([exe,font],input=b''.join(struct.pack('<I',c) for c,_ in entries))
    glyphs = []
    pos = 0
    for _,advance in entries:
        w,h,left,bottom = struct.unpack_from('<BBbb',stream,pos)
        pos += 4
        bits = bytearray((w*h+3)//4)
        for i,value in enumerate(stream[pos:pos+w*h]):
            bits[i//4] |= (value>>6) << (6-2*(i%4))
        pos += w*h
        glyphs.append((2*advance,w,h,left,bottom,bits))
    assert pos==len(stream)
    cmap = b''.join(struct.pack('<H',c) for c,_ in entries)
    directory = (128+len(cmap)+3)&~3
    blocks = (len(entries)+63)//64
    data = bytearray(directory+16*blocks)
    data[128:128+len(cmap)] = cmap
    raw_offset = 0
    for i in range(blocks):
        metrics, pixels = bytearray(), bytearray()
        for advance,w,h,left,bottom,bits in glyphs[i*64:i*64+64]:
            metrics.extend(struct.pack('<HHBBbb',len(pixels),advance,w,h,left,bottom))
            pixels.extend(bits)
        raw = metrics+pixels
        packed = zlib.compress(raw,9)
        struct.pack_into('<IIII',data,directory+16*i,len(data),len(packed),len(raw),raw_offset)
        data.extend(packed)
        raw_offset += len(raw)
    struct.pack_into('<8sIIIIII',data,0,b'MSF32A2\0',len(data),len(entries),directory,blocks,raw_offset,128)
    return data

if __name__=='__main__':
    args=argparse.ArgumentParser(description=__doc__)
    args.add_argument('--font',required=True,type=Path)
    args.add_argument('--output',required=True,type=Path)
    args.add_argument('--freetype-prefix',type=Path,help='FreeType installation prefix if pkg-config is unavailable')
    opts=args.parse_args()
    opts.output.write_bytes(generate(opts.font,opts.freetype_prefix))
