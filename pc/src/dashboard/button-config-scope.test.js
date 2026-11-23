import test from "node:test";
import assert from "node:assert/strict";
import { requireButtonReadback } from "../lib/button-readback.js";
import { readFileSync } from "node:fs";

// Run the production callback body, with transport/setState boundaries stubbed.
// No copied decision logic, React mount, Tauri invocation or physical device.
const source = readFileSync(new URL("../DeviceDashboard.jsx", import.meta.url), "utf8");
const prefix = "const refreshButtonConfigFromBoard = useCallback(async () => {";
const start = source.indexOf(prefix);
assert.ok(start >= 0);
const end = source.indexOf("\n  }, [", start);
assert.ok(end > start);
const body = source.slice(start + prefix.length, end);

function fixture({ matches = false, delayed = false, systemSettings = true, queryReply } = {}) {
  let acknowledge, started;
  const entered = new Promise(resolve => { started = resolve; });
  const ack = new Promise(resolve => { acknowledge = resolve; });
  const events = [], writes = [];

  const queryToken = { current: 0 }, revision = { current: 1 }, target = { current: "board/connection/boot" };
  const bindings = {
    usb: { connected: true, runtime: "esp-p4", capabilities: { systemSettings } }, hasKnownRuntime: true,
    requireButtonReadback, isP4Runtime: true, SETTINGS_NOT_READY: "系统设置暂未生效",
    buttonConfigTargetBoardDeviceId: "board", buttonConfigQueryTokenRef: queryToken,
    buttonConfigRevisionRef: revision, voiceTarget: { key: target.current }, voiceTargetKeyRef: target,
    voiceConfigRef: { current: { enabled: true, buttonActions: { p4_sw3_long: "system_settings" } } }, normalizeVoiceConfig: x => x,
    buttonConfigForDevice: config => config,
    resolveButtonConfigForRuntime: config => ({ config, voiceButton: "sw2", voiceEnabled: true }),
    dispatchBoardButtonConfig: async input => { writes.push(input); throw Error("automatic write"); },
    invoke: async command => { assert.equal(command, "usb_get_button_config"); started(); if (delayed) await ack; return queryReply ?? { ok:true,boardDeviceId:"board",bindings:[{event:"button.sw3.short_press",action:"page_back"}] }; },
    boardButtonConfigMatchesClient: () => matches, buttonDifferenceMessage: () => "采用设备配置",
    setVoiceConfigOtaState: x => events.push(["state", x]),
    setVoiceConfigDirty: x => events.push(["dirty", x]),
    setButtonConfigHydratedFor: x => events.push(["hydrated", x]),
  };
  const run = new Function(...Object.keys(bindings), `return async function () {${body}\n}`)(...Object.values(bindings));
  return { run, entered, acknowledge, events, writes, queryToken, revision, target };
}

for (const change of ["query", "revision", "connection"]) {
  test(`discard old button read after ${change} changes`, async () => {
    const f = fixture({ delayed: true });
    const pending = f.run();
    await f.entered;
    if (change === "query") f.queryToken.current++;
    if (change === "revision") f.revision.current++;
    if (change === "connection") f.target.current = "other-board/new-connection";
    const count = f.events.length;
    f.acknowledge();
    await pending;
    assert.equal(f.events.length, count);
    assert.equal(f.writes.length, 0);
  });
}

test("a valid different mapping on connection requires an explicit choice", async () => {
  const f = fixture();
  await f.run();
  assert.equal(f.writes.length, 0);
  assert.ok(f.events.some(([type, value]) => type === "state" && value.message.includes("采用设备配置")));
  assert.ok(!f.events.some(([type, value]) => type === "hydrated" && value));
});

test("a matching current button map is read-only and hydrates without rewriting", async () => {
  const f = fixture({ matches: true });
  await f.run();
  assert.equal(f.writes.length, 0);
  assert.deepEqual(f.events.at(-1), ["hydrated", "board"]);
});

test("matching supported bindings still show a deferred system settings action", async () => {
  const f = fixture({ matches: true, systemSettings: false });
  await f.run();
  assert.equal(f.writes.length, 0);
  assert.ok(f.events.some(([type, value]) => type === "state" && value.tone === "warning"));
});

test("a malformed or foreign USB query cannot trigger client-authoritative repair", async () => {
  for (const queryReply of [{}, {ok:true,boardDeviceId:"other",bindings:[{event:"e",action:"a"}]}]) {
    const f=fixture({queryReply});
    await f.run();
    assert.equal(f.writes.length,0);
    assert.ok(!f.events.some(([type])=>type==="hydrated"));
    assert.ok(f.events.some(([type,value])=>type==="state" && value.message.includes("已停止自动写入")));
  }
});

test("adopting device buttons re-reads the selected device and never writes to the board", async () => {
  const prefix = "const adoptDeviceButtons = async () => {";
  const start = source.indexOf(prefix) + prefix.length;
  const body = source.slice(start, source.indexOf("\n  };", start));
  for (const wireless of [true, false]) {
    for (const stale of [true, false]) {
      const saved = [], reads = [], state = {ok:true, boardDeviceId:"b", bindings:[{event:"e",action:"page_back"}]};
      const scope = {current:"target"};
      const read = async op => { reads.push(op); if (stale) scope.current = "other"; return state; };
      const deps = {
        usb:{connected:!wireless,boardDeviceId:"b",runtime:"esp-p4"}, wifiInputTarget:{boardDeviceId:"b"},
        buttonConfigRevisionRef:{current:1}, wifiInputScope:"target", voiceTarget:{key:"target"},
        voiceTargetKeyRef:scope, wifiInputScopeRef:scope, setVoiceConfigOtaState:()=>{},
        wifiDeviceControl:(_invoke,_wifi,op)=>read(op), invoke:op=>read(op),
        isP4Runtime:true, requireButtonReadback, P4_RUNTIME_ID:"esp-p4",voiceConfigRef:{current:{}},
        mergeBoardButtonConfig:(_config,_state,_runtime,migrate)=>{ assert.equal(migrate,false); return state; },
        boardButtonConfigMatchesClient:()=>true, saveVoiceConfigToStorage:value=>saved.push(value),
        setVoiceConfig:()=>{},setVoiceConfigDirty:()=>{},setButtonConfigHydratedFor:()=>{},
      };
      await new Function(...Object.keys(deps), `return (async()=>{${body}})()`)(...Object.values(deps));
      assert.deepEqual(reads,[wireless?"readInput":"usb_get_button_config"]);
      assert.deepEqual(saved,stale?[]:[state]);
    }
  }
});
