"""Capture read-only HIL observations, or evaluate a mixed workload offline."""
import argparse
from collections import Counter
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import time

from p4_image_compat import image_application_version, image_revision_range
from preserving_ota import Link
from runtime_memory_probe import sanitize

COMMON = ('appearance', 'animation', 'button', 'audio-exit', 'usb-reconnect')
WIRELESS = ('voice', 'wifi-loss', 'router-restart', 'usb-takeover')
RECOVERIES = ('usb-reconnect', 'wifi-loss', 'router-restart', 'usb-takeover')
PROFILES = ('wifi6-v1', 'wifi6-v3', 'pico-v1', 'pico-v3')


def natural(n):
    return type(n) is int and n >= 0


def observation(identity, diagnostics):
    sample = sanitize(diagnostics)
    runtime = diagnostics['runtime']
    if (identity.get('engine') != 'p4-evidence-identity-v1'
            or diagnostics['boardDeviceId'] != identity.get('boardDeviceId')
            or runtime.get('buildId') != identity.get('buildId')
            or not re.fullmatch('[0-9a-f]{16}', identity.get('bootId', ''))
            or not re.fullmatch('[0-9a-f]{64}', identity.get('appElfSha256', ''))):
        raise ValueError('Device identity mismatch')
    return dict(hostMs=time.time_ns() // 1000000, fingerprint=sample['boardFingerprint'],
                bootId=identity['bootId'], elf=identity['appElfSha256'],
                board=identity.get('hardwareBoardModel'), firmware=runtime['firmware'],
                revision=runtime.get('chipRevision'), panel=runtime.get('lcdPanel'),
                uptimeMs=runtime.get('uptimeMs'), bootCount=sample['bootCount'],
                faultCount=sample['faultResetCount'], freeMin=sample['memory']['minimumFreeInternalBytes'],
                freeDma=sample['memory']['freeDmaBytes'],
                renderGapUs=runtime.get('renderLifetime', {}).get('maxGapUs'))


def capture(args):
    if not 1 <= args.samples <= 3000 or not 1 <= args.interval <= 60:
        raise ValueError('Use 1..3000 samples and a 1..60 second interval')
    # Refuse a busy port. A client that owns USB can also call observation()
    # on the same correlated replies and append them to its own JSONL segment.
    import serial
    ports = [args.port]
    if sys.platform == 'darwin' and args.port.startswith(('/dev/cu.', '/dev/tty.')):
        suffix = args.port.split('.', 1)[1]
        ports = [f'/dev/{prefix}.{suffix}' for prefix in ('cu', 'tty')]
    busy = subprocess.run(['lsof', '-nP', *ports], capture_output=True)
    if busy.returncode != 1 or busy.stderr or busy.stdout:
        raise ValueError('Port busy or occupancy unknown')
    class PassiveLink(Link):
        def __init__(self):
            # A USB-UART bridge can wire DTR/RTS to reset or boot select.
            # Suppress pyserial's line-control ioctls during open.
            class PassiveSerial(serial.Serial):
                def _update_dtr_state(self):
                    pass

                def _update_rts_state(self):
                    pass

            self.s = PassiveSerial(port=None, baudrate=4_000_000, timeout=.15,
                                   write_timeout=2, exclusive=True)
            self.s.port = args.port
            self.s.open()
            try:
                if sys.platform == 'darwin':
                    import fcntl
                    import termios
                    fcntl.ioctl(self.s.fileno(), termios.TIOCEXCL)
                owners = subprocess.run(['lsof', '-nP', '-t', *ports], capture_output=True)
                # lsof also returns 1 when just the unused tty alias has no owner.
                if (owners.returncode not in (0, 1) or owners.stderr or
                        set(owners.stdout.split()) != {str(os.getpid()).encode()}):
                    raise ValueError('Port ownership changed')
            except BaseException:
                self.s.close()
                raise

    with args.output.open('x') as out:
        link = PassiveLink()
        try:
            for i in range(args.samples):
                identity = link.request('debug/identity', {}, 'debug/identity')
                status = link.request('diagnostics/query', {}, 'diagnostics/status')
                print(json.dumps(observation(identity, status)), file=out, flush=True)
                if i + 1 < args.samples:
                    time.sleep(args.interval)
        finally:
            link.s.close()


def assess(profile, image, samples, witness):
    board, family = profile.split('-')
    version = image_application_version(image)
    revisions = image_revision_range(image)
    elf = image[176:208].hex()
    if (not version.endswith('-' + board) or revisions != ((1, 199) if family == 'v1' else (300, 399))
            or len(elf) != 64 or elf == '0' * 64):
        raise ValueError('Candidate does not match selected hardware profile')
    issues = []
    minimum_span = (8 * 3600 if board == 'wifi6' else 15 * 60) * 1000
    result = dict(profile=profile, imageSha256=hashlib.sha256(image).hexdigest(), expectedElf=elf,
                  requiredSpanMs=minimum_span, samples=len(samples), issues=issues)
    if not samples:
        return dict(result, status='pending-device')
    first = samples[0]
    panel = witness.get('panel')
    if panel not in ('ST7701S', 'NV3049F'):
        issues.append('missing witnessed panel')
    ids = ('fingerprint', 'bootId', 'bootCount', 'faultCount', 'revision')
    if (not re.fullmatch('[0-9a-f]{16}', str(first.get('fingerprint', '')))
            or not re.fullmatch('[0-9a-f]{16}', str(first.get('bootId', '')))
            or any(not natural(first.get(k)) for k in ('bootCount', 'faultCount', 'revision'))
            or not revisions[0] <= first['revision'] <= revisions[1]
            or any(any(s.get(k) != first.get(k) for k in ids) or s.get('board') != board
                   or s.get('elf') != elf or s.get('firmware') != version
                   or s.get('panel') not in (None, panel) for s in samples)):
        issues.append('hardware, image, panel or boot mismatch')
    if any(witness.get(k) != first.get(k) or not witness.get(k) for k in ('fingerprint', 'bootId', 'elf')):
        issues.append('operator record identity mismatch')
    for key in ('uptimeMs', 'hostMs'):
        values = [s.get(key) for s in samples]
        if (not all(natural(v) for v in values)
                or any(not 0 < b-a <= 120000 for a, b in zip(values, values[1:]))
                or values[-1] - values[0] < minimum_span):
            issues.append(f'{key}: short or discontinuous')
        elif key == 'uptimeMs':
            result['spanMs'] = values[-1] - values[0]
    if all(natural(s.get(k)) for s in samples for k in ('uptimeMs', 'hostMs')):
        if any(abs((s['hostMs']-first['hostMs']) - (s['uptimeMs']-first['uptimeMs'])) > 60000
               for s in samples):
            issues.append('host and device elapsed times disagree')
    for key, reducer in [('freeMin', min), ('freeDma', min), ('renderGapUs', max)]:
        values = [s.get(key) for s in samples]
        if not all(natural(v) for v in values):
            issues.append(f'missing {key}')
        else:
            result[key] = reducer(values)
    required = COMMON + (WIRELESS if board == 'wifi6' else ())
    counts = Counter()
    active_hours = set()
    recovery = []
    audio_underruns = 0
    prior = -1
    for event in witness.get('events', []):
        kind, at = event.get('kind'), event.get('uptimeMs')
        if (kind not in required or not natural(at) or at <= prior or event.get('ok') is not True
                or not natural(first.get('uptimeMs')) or not natural(samples[-1].get('uptimeMs'))
                or not first['uptimeMs'] <= at <= samples[-1]['uptimeMs']):
            issues.append('failed, duplicate, unordered or unobserved workload event')
            break
        prior = at
        counts[kind] += 1
        active_hours.add((at - first['uptimeMs']) // 3600000)
        if kind in RECOVERIES:
            if not natural(event.get('recoveryMs')):
                issues.append('missing recovery measurement')
            else:
                recovery.append(event['recoveryMs'])
        if kind in ('voice', 'audio-exit'):
            if not natural(event.get('underruns')):
                issues.append('missing per-stream underrun measurement')
            else:
                audio_underruns += event['underruns']
    if (any(not counts[k] for k in required) or board == 'wifi6' and counts['appearance'] < 100):
        issues.append('incomplete workload')
    if board == 'wifi6' and not set(range(8)) <= active_hours:
        issues.append('missing mixed workload observations in one or more hours')
    if natural(witness.get('maxVisibleGapMs')):
        result['maxVisibleGapMs'] = witness['maxVisibleGapMs']
    else:
        issues.append('missing observed display gap')
    if witness.get('unrecoveredDisplays') != 0 or not natural(witness.get('unrecoveredDisplays')):
        issues.append('unrecovered display or missing observation')
    if audio_underruns:
        issues.append('audio underruns observed')
    result.update(panel=panel, events=dict(counts), audioUnderruns=audio_underruns if
                  counts['audio-exit'] and 'missing per-stream underrun measurement' not in issues else None,
                  recoveryP95Ms=sorted(recovery)[math.ceil(.95 * len(recovery))-1] if recovery else None)
    return dict(result, status='failed-or-incomplete' if issues else 'passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    live = sub.add_parser('capture')
    live.add_argument('--port', required=True)
    live.add_argument('--output', type=Path, required=True)
    live.add_argument('--samples', type=int, default=481)
    live.add_argument('--interval', type=int, default=60)
    check = sub.add_parser('assess')
    check.add_argument('profile', choices=PROFILES)
    check.add_argument('--image', required=True, type=Path)
    check.add_argument('--samples', type=Path, nargs='*', default=[])
    check.add_argument('--witness', type=Path)
    args = parser.parse_args()
    if args.command == 'capture':
        capture(args)
        return 0
    samples = [json.loads(line) for path in args.samples for line in path.read_text().splitlines()]
    result = assess(args.profile, args.image.read_bytes(), samples,
                    json.loads(args.witness.read_text()) if args.witness else {})
    print(json.dumps(result, indent=2))
    return 0 if result['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
