"""Verify every shipped launcher tile against its actual desktop package cover."""
import hashlib
import json
import re
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_factory_covers_match_desktop_packages_and_decode_independently():
    config = json.loads((ROOT / 'factory-config.json').read_text())['components']
    art = ROOT / 'assets/component-center'
    blob = (art / 'center_thumbs.deflate').read_bytes()
    header = (art / 'catalog.h').read_text()
    rows = re.findall(r'\{"([^"]+)", "[^"]+", true, (\d+), (\d+)\}, /\* ([0-9a-f]{64}) \*/', header)
    assert [row[0] for row in rows] == config['ids']
    cursor = 0
    for ident, offset, size, source_hash in rows:
        directory = config['firmwareOnlyPackagesDir'] if ident in config.get('firmwareOnlyIds', ()) else config['packagesDir']
        cover = (ROOT / directory / ident / 'pc/cover.png').read_bytes()
        assert hashlib.sha256(cover).hexdigest() == source_hash, ident
        assert int(offset) == cursor and int(size) > 0
        cursor += int(size)
        decoder = zlib.decompressobj()
        frame = decoder.decompress(blob[int(offset):cursor], 1569)
        assert decoder.eof and not decoder.unused_data and len(frame) == 1568, ident
        palette = struct.unpack_from('<16H', frame)
        used = {palette[index] for pair in frame[32:] for index in (pair >> 4, pair & 15)}
        assert len(used) >= 2, f'{ident} has a blank launcher thumbnail'
    assert cursor == len(blob)
