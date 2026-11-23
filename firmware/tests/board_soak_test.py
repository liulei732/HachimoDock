import hashlib
import json
from pathlib import Path
import struct
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from board_soak import COMMON, WIRELESS, RECOVERIES, assess
from ota_matrix import REVISIONS, compare


def image(board, family):
    data = bytearray(256)
    data[0] = 0xe9
    struct.pack_into('<H', data, 12, 18)
    struct.pack_into('<HH', data, 15, *REVISIONS[family])
    data[32:36] = bytes.fromhex('3254cdab')
    version = 'test-' + board
    data[48:48+len(version)] = version.encode()
    data[176:208] = b'X' * 32
    return bytes(data)


def workload(board, family):
    data = image(board, family)
    identity = dict(elf=data[176:208].hex(), fingerprint='0123456789abcdef', bootId='0'*15+'1')
    wireless = board == 'wifi6'
    minutes = 480 if wireless else 15
    samples = [dict(identity, hostMs=500000+i*60000, uptimeMs=i*60000, bootCount=2, faultCount=0,
                    revision=REVISIONS[family][0], panel='ST7701S', freeMin=10240, freeDma=8000,
                    renderGapUs=10000, board=board, firmware='test-'+board) for i in range(minutes+1)]
    kinds = list(COMMON) + (list(WIRELESS) + ['appearance']*99 if wireless else [])
    events = []
    for i, kind in enumerate(kinds):
        event = dict(kind=kind, ok=True, uptimeMs=(i+1)*minutes*60000//len(kinds))
        if kind in RECOVERIES:
            event['recoveryMs'] = 51
        if kind in ('voice', 'audio-exit'):
            event['underruns'] = 0
        events.append(event)
    return data, samples, dict(identity, panel='ST7701S', events=events, unrecoveredDisplays=0,
                               maxVisibleGapMs=200)


@pytest.mark.parametrize('board', ['wifi6', 'pico'])
@pytest.mark.parametrize('family', REVISIONS)
def test_mixed_witness_required_for_each_image(board, family):
    profile = board+'-'+family
    data, samples, witness = workload(board, family)
    assert assess(profile, data, [], {})['status'] == 'pending-device'
    assert assess(profile, data, samples, {})['status'] != 'passed'
    result = assess(profile, data, samples, witness)
    assert result['status'] == 'passed' and result['recoveryP95Ms'] == 51
    samples[1]['elf'] = 'f'*64
    assert assess(profile, data, samples, witness)['status'] != 'passed'


@pytest.mark.parametrize('problem', ['gap', 'restart', 'underflow', 'display', 'duplicate', 'short', 'switches'])
def test_incomplete_and_adverse_runs_fail(problem):
    data, samples, witness = workload('wifi6', 'v3')
    if problem == 'gap':
        del samples[3:7]
    elif problem == 'restart':
        samples[-1]['bootCount'] += 1
    elif problem == 'underflow':
        next(e for e in witness['events'] if e['kind'] == 'voice')['underruns'] = 4
    elif problem == 'display':
        witness['unrecoveredDisplays'] = 1
    elif problem == 'duplicate':
        witness['events'].append(witness['events'][-1])
    elif problem == 'short':
        samples.pop()
    else:
        witness['events'].pop()
    assert assess('wifi6-v3', data, samples, witness)['status'] != 'passed'


def test_release_comparison_checks_bytes_and_bounds(tmp_path):
    release = dict(desktopVersion='test', firmware=[])
    for board in ['wifi6', 'pico']:
        for lo, hi in REVISIONS.values():
            release['firmware'].append(dict(boardModel=board, minChipRevision=lo,
                                            maxChipRevision=hi, bytes=500))
    data = image('wifi6', 'v1')
    binary = tmp_path / 'wifi6-v1-firmware.bin'
    binary.write_bytes(data)
    folder = tmp_path / 'wifi6-v1'
    folder.mkdir()
    record = dict(board='wifi6', family='v1', commit='test', dirty=True, files={
        'firmware.bin': {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}})
    for margin, status, target in [(-1, 'fail', False), (0, 'pass', False), (65536, 'pass', True)]:
        headroom = 131072 + margin
        record['budget'] = dict(otaBytes=len(data)+headroom, appBytes=len(data), otaHeadroomBytes=headroom)
        (folder / 'manifest.json').write_text(json.dumps(record))
        rows = compare(release, tmp_path, tmp_path)['boards']
        assert rows['pico-v3']['status'] == 'pending'
        assert rows['wifi6-v1']['deltaBytes'] == len(data)-500
        assert (rows['wifi6-v1']['status'], rows['wifi6-v1']['targetMet']) == (status, target)
    binary.write_bytes(image('wifi6', 'v3'))
    with pytest.raises(ValueError):
        compare(release, tmp_path, tmp_path)


def test_capture_busy_tty_alias_never_opens_port(tmp_path, monkeypatch):
    from types import SimpleNamespace
    import board_soak
    monkeypatch.setattr(board_soak.sys, 'platform', 'darwin')
    monkeypatch.setitem(sys.modules, 'serial', SimpleNamespace())
    def occupied(argv, capture_output):
        assert argv[-2:] == ['/dev/cu.fixture', '/dev/tty.fixture']
        return SimpleNamespace(returncode=0, stdout=b'occupied', stderr=b'')
    monkeypatch.setattr(board_soak.subprocess, 'run', occupied)
    path = tmp_path / 'record'
    with pytest.raises(ValueError, match='busy'):
        board_soak.capture(SimpleNamespace(samples=3, interval=1, port='/dev/cu.fixture', output=path))
    assert not path.exists()


@pytest.mark.parametrize('other', [True, False])
def test_capture_rechecks_alias_owners_and_releases_port(tmp_path, monkeypatch, other):
    from types import SimpleNamespace
    fcntl = pytest.importorskip("fcntl")
    import board_soak
    ports = []
    class Serial:
        def __init__(self, **kwargs):
            assert kwargs['exclusive'] is True
            self.closed = False
            ports.append(self)
        def open(self):
            self._update_dtr_state()
            self._update_rts_state()
        def fileno(self):
            return 7
        def close(self):
            self.closed = True
    monkeypatch.setattr(board_soak.sys, 'platform', 'darwin')
    monkeypatch.setitem(sys.modules, 'serial', SimpleNamespace(Serial=Serial))
    monkeypatch.setattr(fcntl, 'ioctl', lambda *args: None)
    monkeypatch.setattr(board_soak.subprocess, 'run', lambda argv, capture_output: SimpleNamespace(
        returncode=1, stderr=b'', stdout=((str(board_soak.os.getpid()) + ('\n999999' if other else '')).encode()
                                         if '-t' in argv else b'')))
    requests = []
    monkeypatch.setattr(board_soak.Link, 'request', lambda _, topic, *args: requests.append(topic))
    monkeypatch.setattr(board_soak, 'observation', lambda *args: {'fixture': True})
    args = SimpleNamespace(samples=1, interval=1, port='/dev/cu.fixture', output=tmp_path/'capture')
    if other:
        with pytest.raises(ValueError, match='ownership changed'):
            board_soak.capture(args)
        assert not requests
    else:
        board_soak.capture(args)
        assert requests == ['debug/identity', 'diagnostics/query']
    assert ports[0].closed
