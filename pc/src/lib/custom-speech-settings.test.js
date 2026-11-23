import test from "node:test";
import assert from "node:assert/strict";
import { hasStageKey, readSpeechSnapshot, speechForm, speechRequestUrl, speechSaveInput, updateSpeechField } from "./custom-speech-settings.js";

const empty = { baseUrl: "", model: "", voice: "", active: false, configured: false };
const old = { ...empty, baseUrl: "https://one.invalid/path/v1", model: "model-a", configured: true, active: true };
test("custom speech accepts a first ASR or first TTS without credentials for the other stage", () => {
  const form = { baseUrl: "https://one.invalid", model: " model ", apiKey: " new-key ", voice: " voice-a " };
  for (const stage of ["asr", "tts"]) {
    assert.deepEqual(speechSaveInput(stage, updateSpeechField(stage, form, "voice", form.voice, empty), empty), {
      stage, baseUrl: form.baseUrl, model: "model", apiKey: "new-key", ...(stage === "tts" ? {voice:"voice-a"} : {}),
    });
    assert.throws(() => speechSaveInput(stage, {...form, apiKey:""}, empty), /API Key/);
  }
});

test("custom speech compares the final stage request URL including protocol path", () => {
  for (const [stage,suffix] of [["asr","audio/transcriptions"],["tts","audio/speech"]]) {
    const base = speechForm(old);
    for (const url of [old.baseUrl, old.baseUrl+"/", "https://one.invalid/path", old.baseUrl+"/"+suffix]) {
      const form = {...base,baseUrl:url};
      assert.equal(hasStageKey(stage,form,old),true);
      assert.equal(Object.hasOwn(speechSaveInput(stage,form,old),"apiKey"),false);
    }
    for (const baseUrl of ["https://one.invalid/other/v1","https://two.invalid/path/v1",old.baseUrl+"/chat/completions"]) {
      const moved = updateSpeechField(stage,{...base,apiKey:"typed-key"},"baseUrl",baseUrl);
      assert.equal(moved.apiKey,"");
      assert.throws(()=>speechSaveInput(stage,moved,old),/API Key/);
    }
    for (const host of ["api.xiaomimimo.com", "token-plan-cn.xiaomimimo.com"]) {
      assert.equal(speechRequestUrl(stage,"https://"+host),"https://"+host+"/v1/chat/completions");
      assert.equal(speechRequestUrl(stage,"https://"+host+"/v1/"+suffix),"https://"+host+"/v1/"+suffix);
    }
  }
});

test("custom speech validates remote and loopback endpoints before submitting credentials", () => {
  for (const url of ["https://k@host.invalid", "ftp://host.invalid", "http://remote.invalid",
    "https://host.invalid/v1?key=x", "https://host.invalid/v1#", "http://127.example.invalid"]) {
    assert.equal(speechRequestUrl("asr",url),"");
  }
  for (const url of ["http://127.0.0.2:2333","http://localhost","http://[::1]:4040"]) {
    assert.equal(speechRequestUrl("asr",url),url+"/v1/audio/transcriptions");
  }
  assert.throws(()=>speechSaveInput("asr",{...speechForm(old),model:" "},old),/模型 ID/);
  assert.equal(updateSpeechField("asr",{...speechForm(old),apiKey:"typed"},"model","changed").apiKey,"typed");
});

test("mixed and migrated snapshots read native active flags and expose only public metadata", () => {
  const tts = {...old,model:"model-b",voice:"voice-b",active:false};
  const status = readSpeechSnapshot({asr:{...old,apiKey:"unwanted"},tts:{...tts,maskedKey:"unwanted"}});
  assert.deepEqual(status,{asr:old,tts});
  assert.equal(speechForm(status.asr).apiKey,"");
  assert.equal(speechForm(status.tts).voice,"voice-b");
  assert.throws(()=>readSpeechSnapshot({asr:old}),/回执不完整/);
});

test("TTS sends only a voice explicitly edited for the destination, including explicit removal", () => {
  const saved = { ...old, voice: "private voice design" };
  const alias = saved.baseUrl + "/audio/speech";
  const edit = (form, field, value) => updateSpeechField("tts", form, field, value, saved);
  const payload = form => speechSaveInput("tts", form, saved);
  const aliasForm = edit(speechForm(saved), "baseUrl", alias);
  assert.equal(aliasForm.voice, saved.voice);
  assert.equal(Object.hasOwn(payload(aliasForm), "voice"), false);
  assert.equal(payload(edit(aliasForm, "voice", "")).voice, "");
  const explicit = edit(aliasForm, "voice", "new voice");
  assert.equal(payload(edit(explicit, "baseUrl", saved.baseUrl)).voice, "new voice");
  for (const url of [saved.baseUrl + "/chat/completions", "https://other.invalid/v1"]) {
    const moved = edit(edit(explicit, "baseUrl", url), "apiKey", "other-key");
    assert.equal(moved.voice, "");
    assert.equal(Object.hasOwn(payload(moved), "voice"), false);
    assert.equal(payload(edit(moved, "voice", saved.voice)).voice, saved.voice);
    assert.equal(Object.hasOwn(payload({...explicit, baseUrl: url, apiKey: "other-key"}), "voice"), false);
    assert.equal(edit(moved, "baseUrl", alias).voice, saved.voice);
  }
});
