#!/usr/bin/env python3
"""Synthesize short original effects as bounded WAV samples or a WASM HAB bank."""
import argparse
import io
import json
import math
from pathlib import Path
import random
import re
import struct
import tempfile
import wave

MAX_BYTES = 30_000
DEFAULT_VOLUME = 0.8
RATE = 16_000
BLOCK_SAMPLES = 320
BLOCK_BYTES = 166
STEP = (7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,
        73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,
        494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,
        2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,
        10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767)
INDEX = (-1,-1,-1,-1,2,4,6,8)
AUDIO = {'.wav', '.hab', '.mp3', '.ogg'}


def number(value, low, high):
    return type(value) in (int, float) and math.isfinite(value) and low <= value <= high


def validated(spec, encoding):
    if not isinstance(spec, dict) or set(spec) != {'cues'} or not isinstance(spec['cues'], list):
        raise ValueError('规格必须是只含 cues 数组的对象')
    cues = spec['cues']
    if not 1 <= len(cues) <= (4 if encoding == 'wav' else 64):
        raise ValueError('WAV 模式需要1–4个音效，HAB 模式需要1–64个音效')
    names = set()
    for cue in cues:
        if (not isinstance(cue, dict) or set(cue) != {'id', 'segments'}
                or not isinstance(cue['id'], str) or not re.fullmatch(r'[A-Za-z0-9_.-]{1,15}', cue['id'])
                or cue['id'] in {'.', '..'} or cue['id'] in names):
            raise ValueError('音效需有唯一的安全ASCII id 和 segments')
        names.add(cue['id'])
        segments = cue['segments']
        if not isinstance(segments, list) or not 1 <= len(segments) <= 32:
            raise ValueError('每个音效需要1–32个声音片段')
        for part in segments:
            if (not isinstance(part, dict) or set(part) not in ({'wave', 'ms', 'hz'}, {'wave', 'ms', 'hz', 'volume'})
                    or part['wave'] not in ('sine', 'triangle', 'noise')
                    or type(part['ms']) is not int or not 10 <= part['ms'] <= 8000
                    or not number(part.get('volume', DEFAULT_VOLUME), 0, 1)
                    or not isinstance(part['hz'], list) or len(part['hz']) != 2
                    or not all(number(hz, 40, 7000) for hz in part['hz'])):
                raise ValueError('片段需要wave=sine/triangle/noise、ms=10–8000、hz=[起频,止频](40–7000)和可选volume=0–1（默认0.8）')
        if sum(part['ms'] for part in segments) > 8000:
            raise ValueError('单个音效不能超过8秒')
    sizes = [sum(p['ms'] for p in cue['segments']) * RATE // 1000 for cue in cues]
    estimated = (sum(44 + n * 2 for n in sizes) if encoding == 'wav' else
                 8 + 8 * len(cues) + sum((n + 319) // 320 * BLOCK_BYTES for n in sizes))
    if estimated > MAX_BYTES:
        raise ValueError(f'音效合计预计{estimated}字节，超过{MAX_BYTES}字节；请缩短片段、减少音效或采用设备合成')
    return cues


def synthesize(cue):
    pcm = []
    rng = random.Random(cue['id'])
    for part in cue['segments']:
        count = part['ms'] * RATE // 1000
        phase = 0.0
        fade = min(80, count // 4)
        for i in range(count):
            frequency = part['hz'][0] + (part['hz'][1] - part['hz'][0]) * i / max(1, count - 1)
            phase += frequency / RATE
            value = (rng.uniform(-1, 1) if part['wave'] == 'noise' else
                     math.sin(2 * math.pi * phase) if part['wave'] == 'sine' else
                     1 - 4 * abs((phase % 1) - 0.5))
            envelope = min(1, i / fade, (count - 1 - i) / fade)
            pcm.append(round(value * envelope * part.get('volume', DEFAULT_VOLUME) * 32767))
    return pcm


def wav_bytes(pcm):
    stream = io.BytesIO()
    with wave.open(stream, 'wb') as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(RATE)
        output.writeframes(struct.pack(f'<{len(pcm)}h', *pcm))
    return stream.getvalue()


def ima_block(samples):
    samples = samples + [0] * (BLOCK_SAMPLES - len(samples))
    predictor = samples[0]
    index = 0
    output = bytearray(struct.pack('<hBBH', predictor, index, 0, BLOCK_SAMPLES))
    codes = []
    for sample in samples[1:]:
        step = STEP[index]
        difference = sample - predictor
        code = 8 if difference < 0 else 0
        difference = abs(difference)
        change = step >> 3
        for bit, amount in ((4, step), (2, step >> 1), (1, step >> 2)):
            if difference >= amount:
                code |= bit
                difference -= amount
                change += amount
        predictor = max(-32768, min(32767, predictor + (-change if code & 8 else change)))
        index = max(0, min(88, index + INDEX[code & 7]))
        codes.append(code)
    for i in range(0, len(codes), 2):
        output.append(codes[i] | ((codes[i + 1] if i + 1 < len(codes) else 0) << 4))
    assert len(output) == BLOCK_BYTES
    return bytes(output)


def hab_bytes(clips):
    encoded = [b''.join(ima_block(pcm[i:i + BLOCK_SAMPLES]) for i in range(0, len(pcm), BLOCK_SAMPLES))
               for pcm in clips]
    offset = 8 + len(clips) * 8
    output = bytearray(b'HAB1' + struct.pack('<I', len(clips)))
    for clip in encoded:
        output += struct.pack('<II', offset, len(clip))
        offset += len(clip)
    return bytes(output) + b''.join(encoded)


def check_budget(paths):
    files = set()
    for path in map(Path, paths):
        if path.is_symlink() or not path.exists():
            raise ValueError('音效预算输入缺失或为符号链接')
        candidates = path.rglob('*') if path.is_dir() else [path]
        for candidate in candidates:
            if candidate.suffix.lower() in AUDIO:
                if candidate.is_symlink() or not candidate.is_file():
                    raise ValueError('音效资源必须是普通文件')
                files.add(candidate.absolute())
    total = sum(path.stat().st_size for path in files)
    if total > MAX_BYTES:
        raise ValueError(f'配套音效合计{total}字节，超过{MAX_BYTES}字节')
    return {'bytes': total, 'maxBytes': MAX_BYTES, 'files': len(files)}


def generate(spec, output, encoding):
    cues = validated(spec, encoding)
    clips = [synthesize(cue) for cue in cues]
    files = ({f"{cue['id']}.wav": wav_bytes(pcm) for cue, pcm in zip(cues, clips)} if encoding == 'wav' else
             {'speech.hab': hab_bytes(clips)})
    total = sum(map(len, files.values()))
    if total > MAX_BYTES:
        raise ValueError('最终音效超过30KB，未写入资源')
    # Validate the bank against the actual package format before changing output.
    if encoding == 'hab':
        from pack_hachiapp import validate_audio
        validate_audio(files['speech.hab'])
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        target = output / name
        if target.is_symlink():
            raise ValueError('不能覆盖符号链接音效')
        with tempfile.NamedTemporaryFile(dir=output, delete=False) as staged:
            staged.write(data)
            temporary = Path(staged.name)
        try:
            temporary.replace(target)
        finally:
            temporary.unlink(missing_ok=True)
    return {'bytes': total, 'maxBytes': MAX_BYTES, 'sampleRate': RATE, 'format': encoding,
            'cues': [{'id': cue['id'], 'cue': i, 'ms': len(pcm) * 1000 // RATE} for i, (cue, pcm) in enumerate(zip(cues, clips))],
            'files': list(files)}


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    sub = cli.add_subparsers(dest='command', required=True)
    create = sub.add_parser('generate')
    create.add_argument('spec', type=Path)
    create.add_argument('output', type=Path)
    create.add_argument('--format', choices=['wav', 'hab'], required=True)
    check = sub.add_parser('check')
    check.add_argument('paths', type=Path, nargs='+')
    args = cli.parse_args()
    try:
        report = (generate(json.loads(args.spec.read_text(encoding='utf-8')), args.output, args.format)
                  if args.command == 'generate' else check_budget(args.paths))
        print(json.dumps({'ok': True, **report}, ensure_ascii=False))
    except (ValueError, OSError, TypeError) as error:
        cli.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
