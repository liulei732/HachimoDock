import assert from "node:assert/strict";
import test from "node:test";
import { boundedLanRead, matchesUsbPeer, pairLanAgent, readRealtimeWifiBoard } from "./lanAgentPairing.js";

test("wired trust eligibility follows the verified board and USB session", () => {
  const usb = { connected: true, connectionId: "17", boardDeviceId: "board-a", bootId: "boot-a" };
  const wifi = { boardDeviceId: "board-a", bootId: "boot-a" };
  assert.equal(matchesUsbPeer(usb, wifi), true);
  assert.equal(matchesUsbPeer({ ...usb, bootId: "" }, wifi), true); // Boot checked by native challenge.
  for (const change of [{ connectionId: "" }, { connected: false }, { boardDeviceId: "board-b" }, { bootId: "restarted" }]) {
    assert.equal(matchesUsbPeer({ ...usb, ...change }, wifi), false);
  }
  assert.equal(matchesUsbPeer(null, wifi), false);
});

test("a timed-out LAN read can be retried while its old reply is still outstanding", async () => {
  let finish;
  const old = new Promise(resolve => { finish = resolve; });
  await assert.rejects(boundedLanRead(old, "expired", 5), /expired/);
  assert.equal(await boundedLanRead(Promise.resolve("fresh"), "expired", 100), "fresh");
  finish("stale");
});

test("a delayed read cannot renew permission when the timeout callback was suspended", async () => {
  const realNow = Date.now;
  try {
    Date.now = () => 100;
    const result = boundedLanRead(Promise.resolve("old"), "expired", 1000);
    Date.now = () => 2000;
    await assert.rejects(result, /expired/);
  } finally {
    Date.now = realNow;
  }
});

test("pairing clears the input before sending the code to native", async () => {
  const ip = "192.0.2.12";
  const code = "0123 4567 89ab cdef 0123 4567 89ab cdef";
  const input = { value: code };
  globalThis.window = {
    __TAURI_INTERNALS__: {
      invoke(command, args) {
        assert.equal(input.value, "");
        assert.equal(command, "lan_agent_pair");
        assert.deepEqual(args, { ip, code });
        return Promise.reject(new Error("fixture: rejected pairing"));
      },
    },
  };
  try {
    await assert.rejects(pairLanAgent(ip, input), /fixture/);
    assert.equal(input.value, "");
  } finally {
    delete globalThis.window;
  }
});

test("voice uses one authenticated snapshot and distinguishes missing pairing from offline", async () => {
  let snapshot = { address: null, status: null };
  const calls = [];
  const previous = globalThis.window;
  globalThis.window = { __TAURI_INTERNALS__: { invoke(command) {
    calls.push(command);
    return Promise.resolve(snapshot);
  } } };
  try {
    assert.equal(await readRealtimeWifiBoard(), "");
    snapshot = { address: "test", status: null };
    await assert.rejects(readRealtimeWifiBoard(), /已保存配对/);
    snapshot.status = { boardDeviceId: "board", wifiConnected: true, wirelessRealtime: true, controlTrusted: false };
    assert.equal(await readRealtimeWifiBoard(), "");
    snapshot.status.controlTrusted = true;
    assert.equal(await readRealtimeWifiBoard(), "board");
    assert.deepEqual(calls, Array(4).fill("lan_connection_snapshot"));
  } finally {
    if (previous === undefined) delete globalThis.window;
    else globalThis.window = previous;
  }
});

test("voice reports the native snapshot error for a saved offline device", async () => {
  const previous = globalThis.window;
  globalThis.window = { __TAURI_INTERNALS__: { invoke: async () => ({
    address: "192.0.2.1", status: null, error: "TCP 连接未建立",
  }) } };
  try {
    await assert.rejects(readRealtimeWifiBoard(), /TCP 连接未建立/);
  } finally {
    if (previous === undefined) delete globalThis.window;
    else globalThis.window = previous;
  }
});

test("voice releases a stalled snapshot after 30 seconds and can read again", async (t) => {
  t.mock.timers.enable({ apis: ["setTimeout", "Date"] });
  const previous = globalThis.window;
  let finish;
  const stalled = new Promise(resolve => { finish = resolve; });
  let reply = stalled;
  globalThis.window = { __TAURI_INTERNALS__: { invoke: () => reply } };
  try {
    const read = readRealtimeWifiBoard();
    t.mock.timers.tick(30_000);
    await assert.rejects(read, /超时/);
    reply = Promise.resolve({ address: "192.0.2.1", status: {
      wifiConnected: true, controlTrusted: true, wirelessRealtime: true, boardDeviceId: "board",
    } });
    assert.equal(await readRealtimeWifiBoard(), "board");
    finish({ address: null, status: null });
  } finally {
    if (previous === undefined) delete globalThis.window;
    else globalThis.window = previous;
  }
});
