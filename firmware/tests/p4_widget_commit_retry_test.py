"""Drive the shared HIL sender and receiver through a memory-backed serial stream."""
from collections import deque
import importlib.util
import json
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest


TOOLS = Path(__file__).resolve().parents[1] / "tools"
DEFERRED = {
    "ok": False,
    "errorCode": "commit_storage_deferred",
    "msg": "component storage is busy; commit has not started; staged package preserved",
}


@pytest.fixture
def wire(monkeypatch):
    monkeypatch.syspath_prepend(str(TOOLS))
    monkeypatch.setitem(sys.modules, "serial", SimpleNamespace(Serial=object))
    spec = importlib.util.spec_from_file_location("test_bench", TOOLS / "local_delights_device.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    class SerialStream:
        def __init__(self):
            self.now = 0
            self.pending = bytearray()
            self.sent = []
            self.acks = deque()
            self.responses = deque()
            self.reply_lag = 0

        def sleep(self, seconds):
            self.now += seconds

        def write(self, data):
            self.pending.extend(data)
            if not self.pending.endswith(b"\n"):
                return
            raw = bytes(self.pending)
            self.pending.clear()
            self.sent.append(raw)
            request = json.loads(raw)
            kind, phase = request["topic"].split("/")
            p = request["payload"]
            ack = {k: p[k] for k in ("transferId", "path", "index", "seq") if k in p}
            ack["phase"] = phase
            response = self.responses.popleft() if self.responses else {"ok": True}
            if response is not None:
                reply = "widget-install-ack" if kind == "widget" else kind + "/ack"
                self.acks.append(json.dumps({"topic": reply, "payload": {**ack, **response}}).encode())

        def readline(self):
            self.sleep(self.reply_lag if self.acks else 0.1)
            return self.acks.popleft() if self.acks else b""

    stream = SerialStream()
    monkeypatch.setattr(module, "time", SimpleNamespace(monotonic=lambda: stream.now, sleep=stream.sleep))
    bench = object.__new__(module.Bench)
    bench.s = stream
    return bench, stream


@pytest.mark.parametrize("notebook", [False, True])
def test_standalone_record_install_retries_only_identical_commit(wire, notebook):
    import record_checkin_hil as hil

    bench, stream = wire
    make_files = hil.notebook_wire_files if notebook else hil.wire_files
    files = make_files("retry-fixture", 2)
    chunks = sum((len(content) + 4095) // 4096 for content in files.values())
    stream.responses.extend([{"ok": True}] * (1 + chunks) + [DEFERRED, DEFERRED, {"ok": True}])
    hil.install(bench, "retry-fixture", 2, make_files)
    messages = [json.loads(line) for line in stream.sent]
    assert [m["topic"] for m in messages] == ["widget/begin"] + ["widget/chunk"] * chunks + ["widget/commit"] * 3
    assert len(set(stream.sent[-3:])) == 1
    assert len({m["payload"]["transferId"] for m in messages}) == 1
    assert len({m["payload"]["packageSha256"] for m in messages}) == 1


@pytest.mark.parametrize("response", [
    {"ok": False, "msg": DEFERRED["msg"]},
    {**DEFERRED, "errorCode": "commit_audio_busy"},
    {**DEFERRED, "errorCode": "commit_storage_interrupted"},
    {**DEFERRED, "msg": "unsupported"},
    {**DEFERRED, "ok": 0},
    {"errorCode": "commit_storage_deferred", "msg": DEFERRED["msg"]},
])
def test_other_or_incomplete_rejection_is_terminal(wire, response):
    bench, stream = wire
    stream.responses.append(response)
    with pytest.raises(AssertionError) as failure:
        bench.transfer("widget", "commit", {"transferId": "fixture"})
    assert len(stream.sent) == 1 and failure.value.args[0]["phase"] == "commit"


@pytest.mark.parametrize("responses", [
    [None], [DEFERRED, None],
    [{**DEFERRED, "transferId": "another-transfer"}], [{**DEFERRED, "phase": "begin"}],
])
def test_missing_or_uncorrelated_ack_stops_without_replay(wire, responses):
    bench, stream = wire
    stream.responses.extend(responses)
    with pytest.raises(TimeoutError):
        bench.transfer("widget", "commit", {"transferId": "fixture"}, timeout=.4)
    assert len(stream.sent) == len(responses)


@pytest.mark.parametrize("kind,phase", [
    ("widget", "begin"), ("widget", "chunk"), ("widget", "delete"),
    ("firmware", "commit"), ("asset", "commit"),
])
def test_deferral_cannot_replay_other_operations(wire, kind, phase):
    bench, stream = wire
    stream.responses.append(DEFERRED)
    with pytest.raises(AssertionError):
        bench.transfer(kind, phase, {"transferId": "fixture", "path": "chunk", "index": 0})
    assert len(stream.sent) == 1


@pytest.mark.parametrize("reply_lag,attempts", [(0, 6), (3.45, 1)])
def test_retry_count_and_elapsed_deadline_both_bound_sends(wire, reply_lag, attempts):
    bench, stream = wire
    stream.reply_lag = reply_lag
    stream.responses.extend([DEFERRED] * 10)
    with pytest.raises(AssertionError) as failure:
        bench.transfer("widget", "commit", {"transferId": "fixture"})
    assert failure.value.args[0]["errorCode"] == "commit_storage_deferred"
    assert len(stream.sent) == attempts and len(set(stream.sent)) == 1

def test_retry_window_does_not_shorten_an_in_progress_commit(wire):
    bench, stream = wire
    stream.reply_lag = 8
    stream.responses.append({"ok": True, "unchanged": True})
    ack = bench.transfer("widget", "commit", {"transferId": "fixture"}, timeout=45)
    assert ack["unchanged"] and len(stream.sent) == 1


def test_unrelated_topic_cannot_authorize_replay(wire):
    bench, stream = wire
    stream.acks.append(json.dumps({"topic": "protocol/ack", "payload": {
        **DEFERRED, "transferId": "fixture", "phase": "commit"}}).encode())
    stream.responses.append(None)
    with pytest.raises(TimeoutError):
        bench.transfer("widget", "commit", {"transferId": "fixture"}, timeout=.4)
    assert len(stream.sent) == 1
