"""Validate the actual optional resource and the device's area filter."""
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[1]


def test_resource_identity_coverage_and_block_bounds():
    data = (ROOT / "assets/fonts/bubble32-v1.fpk").read_bytes()
    header = (ROOT / "main/pet_p4_shared_font_id.h").read_text()
    assert len(data) == 851179 < 1000000
    assert hashlib.sha256(data).digest() == bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})',header))
    magic,size,count,directory,blocks,rawsize,cmap = struct.unpack_from('<8sIIIIII',data)
    assert (magic,size,count,cmap)==(b'MSF32A2\0',len(data),6988,128)
    codes=struct.unpack_from(f'<{count}H',data,cmap)
    assert list(codes)==sorted(set(codes)) and all(ord(c) in codes for c in '中文字体任务完成，精密测量。小恐龙')
    pos=directory+16*blocks
    total=0
    for block in range(blocks):
        start,packed,length,offset=struct.unpack_from('<IIII',data,directory+block*16)
        assert start==pos and offset==total
        chunk=zlib.decompress(data[start:start+packed])
        assert len(chunk)==length
        rows=min(64,count-block*64)
        bits=0
        for i in range(rows):
            glyph,advance,w,h,x,y=struct.unpack_from('<HHBBbb',chunk,8*i)
            assert glyph==bits and w<=48 and h<=48
            bits+=(w*h+3)//4
        assert 8*rows+bits==length
        pos+=packed
        total+=length
    assert (pos,total)==(len(data),rawsize)


def test_device_filter_matches_area_sampling_including_odd_edges(tmp_path):
    source=tmp_path/'filter.c'
    source.write_text(r'''
#include "pet_p4_shared_font.h"
#include <assert.h>
int main(void) {
    uint8_t bits[16]={0x1b,0xe4,0x07,0xc9,0xaf,0x96,0x53,0x32,0xdf};
    pet_shared_glyph_t g={bits,7,5,0,0};
    for(unsigned y=0;y<5;y++) for(unsigned x=0;x<7;x++)
        assert(pet_shared_alpha(&g,x,y,false)==85*pet_shared_level(&g,x,y));
    for(unsigned y=0;y<4;y++) for(unsigned x=0;x<6;x++){
        unsigned sum=0;
        for(unsigned yy=0;yy<4;yy++) for(unsigned xx=0;xx<4;xx++)
            sum+=pet_shared_level(&g,(4*x+xx)/3,(4*y+yy)/3);
        assert(pet_shared_alpha(&g,x,y,true)==(85*sum+8)/16);
    }
}
''')
    exe=tmp_path/'filter'
    subprocess.run(['cc','-std=c11','-Wall','-Werror','-I',str(ROOT/'main'),str(source),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)


def test_resource_delivery_ownership_and_interruption(tmp_path):
    vendor = ROOT / "tests/vendor/cjson"
    exe = tmp_path / "font-delivery"
    miniapp = (ROOT / "main/pet_p4_miniapp.c").read_text()
    start = miniapp.index("bool pet_p4_miniapp_stage_font_chunk(")
    end = miniapp.index("\n}", start) + 2
    (tmp_path / "font_stage.inc").write_text(miniapp[start:end])
    subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-I", str(ROOT / "main"),
                    "-I", str(vendor), "-I", str(tmp_path), str(ROOT / "main/pet_p4_storage_lane.c"),
                    str(ROOT / "tests/p4_font_delivery_host.c"),
                    str(vendor / "cJSON.c"), "-o", str(exe)], check=True)
    font = ROOT / "assets/fonts/bubble32-v1.fpk"
    subprocess.run([str(exe), str(font)], cwd=tmp_path, check=True)
    assert (tmp_path / "bubble32-v1.tmp").read_bytes() == font.read_bytes()
