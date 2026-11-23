import test from "node:test";
import assert from "node:assert/strict";
import { pairedWifiTarget, pairedWifiTargetsEqual } from "../shell/DeviceContext.pure.js";
import { componentReadiness } from "./component-readiness.js";

const target = { boardDeviceId: "p4-0123456789ab", bootId: "abcd1234abcd1234" };
const usb = { ...target, connected: true, capabilities: {
  widgetMicLevel: "microphone-level-v1", widgetBleAccessory: "p4-ble-read-v1",
  widgetCredentialStorageReady: true, globalAudioCues:"p4-global-audio-cues-v1",
} };
const wifi = { ...target, wifiConnected: true, controlTrusted: true, clockSynchronized: true };
function states(input) {
  return Object.fromEntries(componentReadiness({ ...target, ...input }).map(r => [r.id, r.state]));
}

test("current board can request local consent and use a protected vault", () => {
  assert.deepEqual(states({usb, wifi, lastResetReason:"power_on"}), {
    wlan:"已认证连接", clock:"已同步", mic:"可请求本次授权", ble:"可请求本次授权",
    cues:"支持 USB 独立下发", cold:"上次为上电复位", vault:"保护存储已就绪", community:"待设备验签",
  });
  assert.equal(states({usb, wifi:{...wifi, clockSynchronized:false}}).clock, "待校时");
});

test("stale, malformed and untrusted observations convey no target readiness", () => {
  for (const stale of [{boardDeviceId:"p4-other"}, {bootId:"1122334455667788"}]) {
    const s = states({usb:{...usb,...stale},wifi:{...wifi,...stale},lastResetReason:"power_on"});
    assert.equal(s.cold, "待现场核对");
    assert.equal(s.mic, "未取得握手");
    assert.equal(s.wlan, "待确认");
    assert.equal(s.vault, "待确认");
  }
  assert.equal(states({wifi:{...wifi,controlTrusted:false}}).clock, "待确认");
  assert.equal(states({usb:{...usb,connected:false}}).mic, "未取得握手");
  assert.equal(states({bootId:"", usb:{...usb,bootId:""}}).mic, "未取得握手");
});

test("old firmware reports unconfirmed capabilities", () => {
  const s = states({usb:{...usb,capabilities:{voice:true}}});
  assert.equal(s.mic,"固件未声明");
  assert.equal(s.wlan,"待确认");
  assert.equal(s.vault,"待确认");
});

test("trusted LAN propagates affirmative vault evidence and invalidates it when lost", () => {
  const ready = pairedWifiTarget({...wifi,widgetCredentialStorageReady:true});
  const legacy = pairedWifiTarget(wifi);
  assert.equal(states({wifi:ready}).vault,"保护存储已就绪");
  assert.equal(states({wifi:ready}).cues,"需 USB 握手");
  assert.equal(states({wifi:legacy}).vault,"待确认");
  assert.equal(pairedWifiTargetsEqual(ready,legacy),false);
  assert.equal(states({wifi:{...ready,controlTrusted:false}}).vault,"待确认");
  assert.equal(states({wifi:ready,usb:{...usb,capabilities:{widgetCredentialStorageReady:false}}}).vault,"保护存储未就绪");
});

test("only positively identified Pico suppresses radio features and local mic stays independent", () => {
  const read = hardwareBoardModel => states({usb:{...usb,capabilities:{
    ...usb.capabilities,firmwareUpdate:{boardModel:"pico",hardwareBoardModel},
  }}});
  for (const id of ["wlan","clock","ble","community","vault"]) assert.equal(read("pico")[id],"本板不支持");
  assert.equal(read("pico").mic,"可请求本次授权");
  assert.equal(read(undefined).community,"待设备验签");
});

test("display never copies arbitrary handshake, account or diagnostic values", () => {
  const marker="private-value-for-test";
  const rows=componentReadiness({...target, usb:{...usb,capabilities:{widgetMicLevel:marker}}, wifi:{...wifi,address:marker},lastResetReason:marker});
  assert.ok(!JSON.stringify(rows).includes(marker));
});

test("wireless cues require this board and boot, explicit capability and control trust; USB takes priority", () => {
  const ready = pairedWifiTarget({...wifi,globalAudioCues:true});
  assert.equal(states({wifi:ready}).cues,"支持 Wi-Fi 独立下发");
  assert.equal(states({usb,wifi:ready}).cues,"支持 USB 独立下发");
  assert.equal(states({usb:{...usb,capabilities:{}},wifi:ready}).cues,"固件未声明");
  assert.equal(states({wifi:{...ready,controlTrusted:false}}).cues,"需授权 Wi-Fi 控制");
  for (const stale of [{bootId:"0000000000000001"},{boardDeviceId:"p4-000000000001"},{wifiConnected:false}])
    assert.equal(states({wifi:{...ready,...stale}}).cues,"需 USB 握手");
  for (const flag of [undefined,false,"true",1]) {
    const legacy = pairedWifiTarget({...wifi,globalAudioCues:flag,appearanceCapacity:8388608});
    assert.equal(pairedWifiTargetsEqual(ready,legacy),false);
    assert.equal(states({wifi:legacy}).cues,"需 USB 握手");
  }
});
