import test from "node:test";
import assert from "node:assert/strict";
import { appearanceTarget, deliverAppearance, wifiAppearanceProgress } from "./appearance-delivery.js";

const board = "p4-123456789abc";
const boot = "0123456789abcdef";
const wifi = { boardDeviceId: board, bootId: boot, wifiConnected: true, controlTrusted: true, appearanceCapacity: 1000 };
const usb = { boardDeviceId: board, bootId: boot, connectionId: "42", connected: true };

test("appearance delivery pins one live board and boot; a present USB never spills over to Wi-Fi", () => {
  assert.deepEqual(appearanceTarget(usb, wifi, board), { transport: "usb", boardDeviceId: board, connectionId: "42", bootId: boot });
  assert.equal(appearanceTarget({ ...usb, connectionId: "" }, wifi, board), null);
  for (const patch of [{ controlTrusted: false }, { wifiConnected: false }, { appearanceCapacity: 0 }, { bootId: "old" }, { boardDeviceId: "other" }]) {
    assert.equal(appearanceTarget(null, { ...wifi, ...patch }, board), null);
  }
  assert.equal(appearanceTarget(null, wifi, "p4-000000000000"), null);
  assert.deepEqual(appearanceTarget(null, wifi, board), { transport: "wifi", boardDeviceId: board, bootId: boot });
});

test("appearance requests use the exact selected target and never resend an unknown result", async () => {
  for (const reply of ["alreadyActive", "uploaded", "activated", "unknown", new Error("lost ack")]) {
    const calls = [];
    const send = deliverAppearance(async (...args) => {
      calls.push(args);
      if (reply instanceof Error) throw reply;
      return reply;
    }, appearanceTarget(null, wifi, board), "pet", "request-1");
    if (reply === "unknown" || reply instanceof Error) await assert.rejects(send);
    else assert.ok((await send).length);
    assert.deepEqual(calls, [["lan_sync_appearance", { appearanceId: "pet", requestId: "request-1", boardDeviceId: board, bootId: boot }]]);
  }
  const calls = [];
  await assert.rejects(deliverAppearance(async (...args) => { calls.push(args); return { ok: false }; },
    appearanceTarget(usb, wifi, board), "pet", "request-1"));
  assert.deepEqual(calls, [["usb_sync_appearance", { appearanceId: "pet", requestId: "request-1", boardDeviceId: board, expectedConnectionId: "42", expectedBootId: boot }]]);
});

test("Wi-Fi slot checks, local preparation and cached activation never report transferred bytes", () => {
  const owner = { boardDeviceId: board, bootId: boot, requestId: "this-click" };
  for (const stage of ["preparing", "activating"]) {
    const progress = wifiAppearanceProgress({ ...owner, stage, reason: "different_pack", bytesSent: 4, bytesTotal: 100 }, owner);
    assert.equal(progress.indeterminate, true);
    assert.equal(progress.percent, undefined);
  }
  for (const reason of ["different_pack", "custom_absent"]) {
    const progress = wifiAppearanceProgress({ ...owner, stage: "preparing", reason }, owner);
    assert.match(progress.text, reason === "different_pack" ? /版本不同/ : /尚未缓存/);
  }
});

test("only valid acknowledged bytes for this board, boot and click show Wi-Fi transfer progress", () => {
  const owner = { boardDeviceId: board, bootId: boot, requestId: "this-click" };
  const chunk = { ...owner, stage: "transferring", bytesSent: 4096, bytesTotal: 102400 };
  for (const patch of [{ requestId: "previous-click" }, { requestId: null }, { bootId: "old" },
    { boardDeviceId: "other" }, { stage: "slot-query" }, { stage: undefined },
    { bytesSent: 0 }, { bytesTotal: 0 }, { bytesTotal: NaN }, { bytesSent: 102401 }]) {
    assert.equal(wifiAppearanceProgress({ ...chunk, ...patch }, owner), null);
  }
  assert.equal(wifiAppearanceProgress(chunk, owner).percent, 4);
  const done = wifiAppearanceProgress({ ...chunk, bytesSent: chunk.bytesTotal }, owner);
  assert.equal(done.indeterminate, true);
  assert.match(done.text, /校验并激活/);
});
