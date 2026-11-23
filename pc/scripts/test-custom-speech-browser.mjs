// Chromium, isolated React root and simulated native IPC. Run from pc.
import assert from "node:assert/strict";
import { chromium } from "playwright";
import { createServer } from "vite";

const server = await createServer({server:{host:"127.0.0.1",port:0},logLevel:"error"});
await server.listen();
const base = new URL(server.resolvedUrls.local[0]).origin;
let browser;
try {
  browser = await chromium.launch({headless:true,executablePath:process.env.PET_TEST_BROWSER});
  const page = await browser.newPage({serviceWorkers:"block"});
  const errors = [];
  page.on("pageerror",error=>errors.push(error.message));
  await page.route("**/*",route=>{
    const url = new URL(route.request().url());
    if (url.origin !== base) throw Error("Unexpected external request");
    if (url.pathname === "/__custom_speech") return route.fulfill({contentType:"text/html",body:'<div id="root"></div>'});
    return route.continue();
  });
  await page.goto(base+"/__custom_speech");
  await page.evaluate(async()=>{
    const Refresh = (await import("/@react-refresh")).default;
    Refresh.injectIntoGlobalHook(window);
    window.$RefreshReg$ = ()=>{};
    window.$RefreshSig$ = ()=>type=>type;
    window.__vite_plugin_react_preamble_installed__ = true;
    const css = document.createElement("link");
    css.rel="stylesheet"; css.href="/src/styles.css"; document.head.append(css);
    const empty = {baseUrl:"",model:"",voice:"",active:false,configured:false};
    const f = window.fixture = { db:{asr:{...empty},tts:{...empty}}, calls:[], notifications:[], hold:{}, release:{} };
    window.__TAURI_INTERNALS__ = {invoke:async(command,args={})=>{
      f.calls.push({command,args:structuredClone(args)});
      if (command === "load_custom_speech_settings") {
        if(f.failRead) throw Error("fixture storage error");
        return structuredClone(f.db);
      }
      if (command === "save_custom_speech_stage") {
        const {stage,model,baseUrl,voice} = args.input;
        if(f.failSave === stage) throw Error("fixture storage error");
        const audioBase = url => url.replace(/\/audio\/speech$/, "");
        const sameTtsRoute = stage === "tts" && audioBase(baseUrl) === audioBase(f.db.tts.baseUrl);
        const storedVoice = voice ?? (sameTtsRoute ? f.db.tts.voice : "");
        f.db[stage] = {baseUrl,model,voice:storedVoice,active:true,configured:true};
        const reply = structuredClone(f.db);
        if(f.hold[stage]) await new Promise(resolve=>{f.release[stage]=resolve;});
        return reply;
      }
      if (command === "test_custom_speech_stage") {
        if(f.holdProbe === args.stage) await new Promise(resolve=>{f.finishProbe=resolve;});
        if(f.fail === args.stage) throw Error("HTTP 403 private-fixture-body");
        return {stage:args.stage,latencyMs:args.stage === "tts" ? 24 : 17};
      }
      throw Error("Unexpected native command "+command);
    }};
    const React = (await import("/node_modules/.vite/deps/react.js")).default;
    const {createRoot} = (await import("/node_modules/.vite/deps/react-dom_client.js")).default;
    const Panel = (await import("/src/CustomSpeechSettings.jsx")).default;
    const root = createRoot(document.getElementById("root"));
    const update = value=>{f.parentState=value;};
    const saved = (stage,snapshot)=>f.notifications.push({stage,snapshot});
    f.render = (active=true,presetRevision=0,instance=0)=>root.render(React.createElement(Panel,{
      key:instance,active,presetRevision,onStateChange:update,onSaved:saved
    }));
    f.render(false);
  });
  const group = stage=>page.getByRole("group",{name:stage==="asr"?"语音识别（ASR）":"语音合成（TTS）"});
  const input = (stage,key)=>group(stage).getByLabel(key,{exact:true});
  const status = stage=>group(stage).getByRole("status");
  const button = (stage,name)=>group(stage).getByRole("button",{name,exact:true});
  const waitText = (stage,text)=>status(stage).filter({hasText:text}).waitFor();
  const fill = async(stage,url,model,key)=>{
    await input(stage,"接口地址").fill(url);
    await input(stage,"模型 ID").fill(model);
    await input(stage,"API Key").fill(key);
  };
  const calls = ()=>page.evaluate(()=>window.fixture.calls);
  assert.equal((await calls()).length,0);
  await page.evaluate(()=>window.fixture.render());
  await waitText("asr","尚未配置");

  // Configure TTS before ASR exists. Model and voice belong to this vendor alone.
  await fill("tts","https://b.invalid/v1","b-voice-model","key-b");
  await input("tts","合成音色").fill("b-voice");
  await button("tts","保存并测试").click();
  await waitText("tts","测试通过（24 ms）");
  await waitText("asr","尚未配置");
  let history = await calls();
  assert.deepEqual(history.map(row=>row.command),["load_custom_speech_settings","save_custom_speech_stage","test_custom_speech_stage"]);
  assert.deepEqual(history[1].args.input,{stage:"tts",baseUrl:"https://b.invalid/v1",model:"b-voice-model",voice:"b-voice",apiKey:"key-b"});
  assert.deepEqual(history[2].args,{stage:"tts"});
  assert.equal(await input("tts","API Key").inputValue(),"");

  // Same URL as the other stage also requires a fresh key on first ASR setup.
  await fill("asr","https://b.invalid/v1","a-listen","");
  await button("asr","仅保存").click();
  await waitText("asr","请填写此接口的 API Key");
  assert.equal((await calls()).length,history.length);
  await fill("asr","https://a.invalid/v1","a-listen","key-a");
  await page.evaluate(()=>{window.fixture.fail="asr";});
  await button("asr","保存并测试").click();
  await waitText("asr",/已保存.*语音识别.*HTTP 403/);
  await waitText("tts","测试通过（24 ms）");
  assert.doesNotMatch(await status("asr").innerText(),/private-fixture/);

  // A changed host or protocol cannot borrow a typed or previously stored key.
  await input("asr","API Key").fill("must-clear");
  await input("asr","接口地址").fill("https://a.invalid/v1/chat/completions");
  assert.equal(await input("asr","API Key").inputValue(),"");
  history=await calls();
  await button("asr","保存并测试").click();
  await waitText("asr","请填写此接口的 API Key");
  assert.equal((await calls()).length,history.length);
  await input("asr","API Key").fill("new-key");
  await page.waitForFunction(()=>window.fixture.parentState?.dirty === true);
  await page.evaluate(()=>window.fixture.render(false));
  await group("asr").waitFor({state:"hidden"});
  await page.evaluate(()=>window.fixture.render(true));
  assert.equal(await input("asr","API Key").inputValue(),"new-key");

  // Concurrent independent saves, including reversed acknowledgements.
  await page.evaluate(()=>{window.fixture.hold.asr=true;});
  await button("asr","仅保存").click();
  await page.waitForFunction(()=>window.fixture.release.asr && window.fixture.parentState.busy);
  await input("tts","合成音色").fill("b-voice-2");
  await button("tts","仅保存").click();
  await waitText("tts","已保存。");
  await page.evaluate(()=>window.fixture.release.asr());
  await waitText("asr","已保存。");
  await page.waitForFunction(()=>window.fixture.parentState.busy===false && window.fixture.parentState.dirty===false);
  const last = await page.evaluate(()=>window.fixture.notifications.at(-1).snapshot);
  assert.equal(last.tts.voice,"b-voice-2");
  assert.equal(last.asr.baseUrl,"https://a.invalid/v1/chat/completions");
  assert.doesNotMatch(JSON.stringify(last),/key-a|key-b|new-key/);

  // Completing the other stage cannot end or replace an in-flight probe.
  await page.evaluate(()=>{window.fixture.holdProbe="asr";});
  await button("asr","测试已保存配置").click();
  await page.waitForFunction(()=>!!window.fixture.finishProbe);
  await button("tts","测试已保存配置").click();
  await waitText("tts","测试通过（24 ms）");
  await waitText("asr","正在测试");
  await page.evaluate(()=>{window.fixture.finishProbe();window.fixture.holdProbe=null;});
  await waitText("asr",/语音识别.*HTTP 403/);
  await waitText("tts","测试通过（24 ms）");

  // Preset refresh leaves a draft intact; test-only does not save or activate.
  await input("tts","合成音色").fill("unsaved");
  await page.evaluate(()=>{
    window.fixture.db.asr.active=false;
    window.fixture.fail=null;
    window.fixture.render(true,1);
  });
  await waitText("asr","配置已保存，当前使用其他服务");
  assert.equal(await input("tts","合成音色").inputValue(),"unsaved");
  history=await calls();
  await button("asr","测试已保存配置").click();
  await waitText("asr","测试通过（17 ms）");
  assert.deepEqual((await calls()).slice(history.length),[{command:"test_custom_speech_stage",args:{stage:"asr"}}]);
  assert.equal(await page.evaluate(()=>window.fixture.db.asr.active),false);

  // A cold mount must reconstruct a mixed selection entirely from native readback.
  const previousForm = await input("tts","合成音色").elementHandle();
  await page.evaluate(()=>window.fixture.render(true,1,"cold-mixed"));
  await page.waitForFunction(el => !el.isConnected, previousForm);
  await waitText("asr","配置已保存，当前使用其他服务");
  await waitText("tts","正在使用此自定义服务");
  assert.equal(await input("asr","接口地址").inputValue(),"https://a.invalid/v1/chat/completions");
  assert.equal(await input("asr","模型 ID").inputValue(),"a-listen");
  assert.equal(await input("tts","接口地址").inputValue(),"https://b.invalid/v1");
  assert.equal(await input("tts","合成音色").inputValue(),"b-voice-2");

  // Reopen with a migrated shared endpoint. Both stage-specific model fields and voice survive.
  await page.evaluate(()=>{
    const f=window.fixture;
    f.db.asr={...f.db.asr,active:true,baseUrl:"https://legacy.invalid/v1",model:"legacy-asr"};
    f.db.tts={...f.db.tts,active:true,baseUrl:"https://legacy.invalid/v1",model:"legacy-tts",voice:"legacy-voice"};
    f.failRead=true;
    f.render(true,1,1);
  });
  await page.getByRole("button",{name:"重新读取"}).waitFor();
  assert.equal(await button("asr","仅保存").isDisabled(),true);
  await page.evaluate(()=>{window.fixture.failRead=false;});
  await page.getByRole("button",{name:"重新读取"}).click();
  for (const stage of ["asr","tts"]) {
    await waitText(stage,"正在使用此自定义服务");
    assert.equal(await input(stage,"模型 ID").inputValue(),"legacy-"+stage);
    assert.equal(await input(stage,"接口地址").inputValue(),"https://legacy.invalid/v1");
    assert.equal(await input(stage,"API Key").inputValue(),"");
  }
  assert.equal(await input("tts","合成音色").inputValue(),"legacy-voice");
  // Check the actual IPC payload for equivalent and different synthesis routes.
  const lastInput = async () => (await calls()).filter(row=>row.command === "save_custom_speech_stage").at(-1).args.input;
  await input("tts","接口地址").fill("https://legacy.invalid/v1/audio/speech");
  assert.equal(await input("tts","合成音色").inputValue(),"legacy-voice");
  await button("tts","仅保存").click();
  await waitText("tts","已保存。");
  assert.equal(Object.hasOwn(await lastInput(),"voice"),false);
  assert.equal(await input("tts","合成音色").inputValue(),"legacy-voice");

  await input("tts","接口地址").fill("https://legacy.invalid/v1/chat/completions");
  assert.equal(await input("tts","合成音色").inputValue(),"");
  await input("tts","API Key").fill("chat-key");
  await button("tts","仅保存").click();
  await waitText("tts","已保存。");
  assert.equal(Object.hasOwn(await lastInput(),"voice"),false);
  assert.equal(await page.evaluate(()=>window.fixture.db.tts.voice),"");

  await fill("tts","https://new.invalid/v1","new-tts","new-tts-key");
  await input("tts","合成音色").fill("explicit-new-design");
  await button("tts","仅保存").click();
  await waitText("tts","已保存。");
  assert.equal((await lastInput()).voice,"explicit-new-design");
  await input("tts","合成音色").fill("");
  await button("tts","仅保存").click();
  await waitText("tts","已保存。");
  assert.equal((await lastInput()).voice,"");
  assert.equal(await page.evaluate(()=>window.fixture.db.tts.voice),"");
  console.log("PASS TTS voice IPC: equivalent URL omits and retains voice, protocol change omits and clears, explicit new voice and explicit removal are transmitted.");
  await page.evaluate(()=>{window.fixture.failSave="tts";});
  await input("tts","模型 ID").fill("retry-model");
  history=await calls();
  await button("tts","保存并测试").click();
  await waitText("tts","保存失败");
  assert.equal((await calls()).length,history.length+1);
  assert.equal(await input("tts","模型 ID").inputValue(),"retry-model");
  // ASR-first setup works with no TTS URL, model or credential in storage.
  await page.evaluate(()=>{
    const f = window.fixture;
    const empty={baseUrl:"",model:"",voice:"",active:false,configured:false};
    f.db={asr:{...empty},tts:{...empty}};
    f.hold={}; f.failSave=null;
    f.render(true,2,2);
  });
  await waitText("asr","尚未配置");
  assert.equal(await input("asr","模型 ID").getAttribute("placeholder"),"faster-whisper-large-v3");
  assert.equal(await input("tts","模型 ID").getAttribute("placeholder"),"cosyvoice-300m-sft");
  history=await calls();
  await fill("asr","https://only-a.invalid/v1","first-asr","key-a-only");
  await button("asr","保存并测试").click();
  await waitText("asr","测试通过（17 ms）");
  await waitText("tts","尚未配置");
  assert.deepEqual((await calls()).slice(history.length).map(row=>row.args.input?.stage || row.args.stage),["asr","asr"]);
  await page.setViewportSize({width:360,height:740});
  assert.ok(await page.evaluate(()=>document.documentElement.scrollWidth <= window.innerWidth));
  assert.deepEqual(errors,[]);
  console.log("PASS Chromium: first ASR, first TTS, independent ASR/vendor/key, endpoint switch, mixed readback, held drafts, separate results, out-of-order saves, legacy shared restore, failed read retry and save failure without probe.");
} finally { await browser?.close(); await server.close(); }
