import test from "node:test";
import assert from "node:assert/strict";
import { testMessageTarget, sendTestMessage } from "./device-test-message.js";
const boardDeviceId = "p4-123456789abc", bootId = "0123456789abcdef";
const wifi = { boardDeviceId, bootId, wifiConnected: true, controlTrusted: true, panelControl: true };
const usb = { boardDeviceId, bootId, connected: true, runtime: "esp-p4", connectionId: "15" };

test("USB wins and unattested targets cannot fall through to wireless", () => {
  assert.equal(testMessageTarget(usb, wifi, "another"), null);
  assert.equal(testMessageTarget({...usb,connectionId:""},wifi),null);
  assert.equal(testMessageTarget(null,{...wifi,controlTrusted:false}),null);
  assert.equal(testMessageTarget({...usb,capabilities:{testMessageReceipt:true}},wifi).connectionId,"15");
});

test("simultaneous guide/menu clicks send once; unknown results are not replayed", async () => {
  const target = testMessageTarget(null,wifi);
  const calls = [];let reject;
  const invoke = (...args) => {calls.push(args);return new Promise((_yes,no)=>{reject=no;});};
  const first = sendTestMessage(invoke,target);
  await assert.rejects(sendTestMessage(invoke,target),/等待/);
  reject(Error("回执未确认"));await assert.rejects(first,/未确认/);
  assert.deepEqual(calls,[["send_device_test_message",{boardDeviceId,bootId,connectionId:null}]]);
});

test("earlier P4 requests a protocol receipt; a legacy USB write is labelled separately", async () => {
  for(const runtime of ["esp-p4","linux"]) {
    const calls=[]; const message=await sendTestMessage(async (...a)=>calls.push(a),testMessageTarget({...usb,runtime},wifi));
    assert.equal(calls.length,1);assert.equal(calls[0][0],"usb_send_speech");
    assert.equal(calls[0][1].requestReceipt,runtime==="esp-p4");
    assert.match(message,runtime==="esp-p4"?/已确认接收/:/已写入 USB/);
  }
});
