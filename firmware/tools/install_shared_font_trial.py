"""Install the pinned MiSans pack on a trial device, reboot and confirm activation."""
import argparse
import base64
import hashlib
import re
from pathlib import Path
import time
import uuid
from preserving_ota import Link

PACK = Path(__file__).resolve().parents[1]/'assets/fonts/bubble32-v1.fpk'
EXPECTED = 'c9dd60d79ed2473a447cab7e1960ff8885192df0ff8bb509492f31b6f25033e7'

def install(link, board):
    def diagnostics():
        response = link.request('diagnostics/query', {}, 'diagnostics/status')
        if response['boardDeviceId'] != board:
            raise RuntimeError('Unexpected device')
        return response
    def command(phase, **kwargs):
        return link.request(topic, dict(resource='bubble32-v1', phase=phase, transferId=tid, **kwargs),
                            topic + '/status', timeout=30)
    data = PACK.read_bytes()
    if hashlib.sha256(data).hexdigest() != EXPECTED:
        raise ValueError('Font pack differs from the reviewed trial resource')
    before = diagnostics()
    # Previously installed R301 bench firmware only exposes its trial command.
    legacy = bool(re.search(r'-r301(?:[-+]|$)', before['runtime']['firmware']))
    topic = 'debug/shared-font' if legacy else 'resources/bubble-font'
    tid = uuid.uuid4().hex
    if command('status')['loaded']:
        print('Resource already loaded')
        return
    storage = before['storage']
    if storage['totalBytes']-storage['usedBytes'] < len(data)+256*1024:
        raise RuntimeError('Insufficient component storage')
    command('begin')
    try:
        for offset in range(0,len(data),3072):
            part = data[offset:offset+3072]
            reply = command('chunk', offset=offset, data=base64.b64encode(part).decode())
            if reply.get('received') != offset+len(part):
                raise RuntimeError('Unexpected chunk acknowledgement')
    except Exception:
        command('abort')
        raise
    receipt = command('commit')
    if not legacy and not receipt.get('loaded'):
        raise RuntimeError('Font activation was not confirmed')
    print('Resource digest verified by device; restarting', flush=True)
    link.request('system/reboot', {}, 'diagnostics/action')
    deadline = time.monotonic()+75
    while time.monotonic()<deadline:
        time.sleep(2)
        try:
            if not command('status')['loaded']:
                continue
            after=diagnostics()
            assert before['runtime']['firmware']==after['runtime']['firmware']
            assert before['faultResetCount']==after['faultResetCount']
            print('Shared font loaded; resource storage delta',after['storage']['usedBytes']-storage['usedBytes'])
            return
        except TimeoutError:
            continue
    raise RuntimeError('Font activation after reboot unconfirmed')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--board',required=True)
    parser.add_argument('--port',required=True)
    opts=parser.parse_args()
    link=Link(opts.port)
    try:
        install(link,opts.board)
    finally:
        link.s.close()
