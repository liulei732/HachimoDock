import test from "node:test";
import assert from "node:assert/strict";
import { componentPetVoice, createComponentVoiceSync } from "./component-pet-voice.js";
test("component speech shares pet voice and clone, not persona or legacy gain", () => {
  const a={id:"pet-a",personaVoice:{persona:{intro:"private"},voice:{speaker:"test",clone_speaker_id:"S_clone",speed:1.2,volume:0.3}}};
  assert.deepEqual(componentPetVoice(a,{connected:true,boardDeviceId:"board"}),{
    boardDeviceId:"board",appearanceId:"pet-a",speaker:"test",cloneSpeakerId:"S_clone",speed:1.2,
  });
  assert.equal(componentPetVoice(a,{connected:false,boardDeviceId:"board"}),null);
  assert.equal(componentPetVoice(null,{connected:true,boardDeviceId:"board"}),null);
});
test("component speech uses the same scoped appearance voice as realtime speech", () => {
  const ttsOverrides = { mimo: { model: "mimo-v2.5-tts", voice: "茉莉" } };
  const appearance = {
    id: "pet-a",
    personaVoice: { persona: { intro: "private" }, voice: { tts_overrides: ttsOverrides } },
  };
  const input = componentPetVoice(appearance, { connected: true, boardDeviceId: "board" });
  assert.deepEqual(input.ttsOverrides, ttsOverrides);
  assert.equal(Object.hasOwn(input, "persona"), false);
});
test("late calls cannot replace the latest pet or disconnected target", async () => {
  const calls=[];let release;
  const sync=createComponentVoiceSync(async(cmd,payload)=>{
    assert.equal(cmd,"component_speech_set_pet_voice");calls.push(payload.input);
    if(calls.length===1)await new Promise(r=>{release=r;});
  });
  const work=sync({appearanceId:"a"});
  void sync({appearanceId:"b"});void sync(null);release();await work;
  assert.deepEqual(calls,[{appearanceId:"a"},null]);
  await sync({appearanceId:"c"});assert.equal(calls.at(-1).appearanceId,"c");
});
test("a failed update can be retried without an unhandled rejection", async()=>{
  let count=0;const sync=createComponentVoiceSync(async()=>{count++;if(count===1)throw Error("offline");});
  await sync(null);await sync(null);assert.equal(count,2);
});

test("component voice follows the verified transport and never falls back from a connected unidentified USB board", () => {
  const appearance = { id: "pet", personaVoice: { voice: { speaker: "voice" } } };
  const wifi = { wifiConnected: true, controlTrusted: true, bootId: "boot-a", boardDeviceId: "board-a" };
  const { route, ...lanVoice } = componentPetVoice(appearance, { connected: false }, wifi);
  const { route: usbRoute, ...usbVoice } = componentPetVoice(appearance, { connected: true, boardDeviceId: "board-a", connectionId: "42" });
  assert.deepEqual(lanVoice, usbVoice);
  assert.deepEqual(route, { wifi: wifi.bootId });
  assert.deepEqual(usbRoute, { usb: "42" });
  assert.equal(componentPetVoice(appearance, { connected: true, boardDeviceId: "board-b" }, wifi).boardDeviceId, "board-b");
  assert.equal(componentPetVoice(appearance, { connected: true }, wifi), null);
  for (const patch of [{ controlTrusted: false }, { bootId: "" }, { boardDeviceId: "" }, { wifiConnected: false }]) {
    assert.equal(componentPetVoice(appearance, null, { ...wifi, ...patch }), null);
  }
});
