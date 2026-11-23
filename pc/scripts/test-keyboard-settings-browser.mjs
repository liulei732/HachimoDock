// Actual settings components and event handlers; replace only the native transport.
import assert from "node:assert/strict";
import { chromium } from "playwright";

const base = process.env.PET_TEST_URL || "http://127.0.0.1:4591";
assert.equal(new URL(base).hostname, "127.0.0.1");
const browser = await chromium.launch({headless:true,
  ...(process.env.PET_TEST_BROWSER ? {executablePath:process.env.PET_TEST_BROWSER} : {})});
try {
  const page = await browser.newPage();
  page.setDefaultTimeout(7000);
  const errors=[];
  page.on("pageerror",e=>{errors.push(e.message);console.error(e.message);});
  await page.route("**/__keyboard-test",r=>r.fulfill({contentType:"text/html",body:'<div id="root"></div>'}));
  await page.goto(base+"/__keyboard-test");
  await page.evaluate(async()=>{
    const refresh=(await import("/@react-refresh")).default;
    refresh.injectIntoGlobalHook(window);
    window.$RefreshReg$=()=>{}; window.$RefreshSig$=()=>type=>type;
    window.__vite_plugin_react_preamble_installed__=true;
    const React=(await import("/node_modules/.vite/deps/react.js")).default;
    const {default:dom}=await import("/node_modules/.vite/deps/react-dom_client.js");
    const {default:Keyboard}=await import("/src/dashboard/KeyboardConfiguration.jsx");
    const root=dom.createRoot(document.querySelector("#root"));
    window.usbTest={connected:true,boardDeviceId:"usb-target",connectionId:"7",bootId:"a".repeat(16),
      capabilities:{bluetoothKeyboardConfig:"p4-ble-keyboard-config-v1",bluetoothKeyboardExtendedChords:true}};
    window.wifiTest={boardDeviceId:"wifi-target",bootId:"b".repeat(16),wifiConnected:true,controlTrusted:true};
    const fixture=id=>({ok:true,enabled:false,connected:false,paired:false,pairing:false,radioState:1,
      boardDeviceId:id,keys:Array(20).fill(0)});
    window.states={"usb-target":fixture("usb-target"),"wifi-target":fixture("wifi-target")};
    window.calls=[]; window.dropEcho=false;
    window.__TAURI_INTERNALS__={invoke:async(cmd,args)=>{
      if(cmd==="set_exit_reasons")return;
      if(!cmd.endsWith("bluetooth_keyboard_config"))throw Error(cmd);
      calls.push({cmd,...args});
      const s=states[args.boardDeviceId];
      if(window.blockRead && args.slot==null && args.enabled==null){
        window.blockRead=false;
        await new Promise(resolve=>window.finishRead=resolve);
      }
      if(args.conditional){
        if ("enabled" in args || "slot" in args || "chord" in args) throw Error("mixed legacy write");
        const update=args.conditional;
        if(window.interleave==="other") s.keys[3]=19;
        if(window.interleave==="mode") s.enabled=!s.enabled;
        if(window.interleave==="slot") s.keys[update.slot]=20;
        window.interleave=null;
        if(window.ignoreConditional) return structuredClone(s);
        if("setChord" in update){
          if(s.keys[update.slot]!==update.expectedChord)throw Error("设备上的键盘设置已变化，请重新核对。");
          s.keys[update.slot]=update.setChord;
        } else {
          if(s.enabled!==update.expectedEnabled)throw Error("设备上的键盘设置已变化，请重新核对。");
          s.enabled=update.setEnabled;
        }
        return {...structuredClone(s),conditionalApplied:true};
      }
      if(args.slot!=null && !dropEcho)s.keys[args.slot]=args.chord;
      if(args.enabled!=null)s.enabled=args.enabled;
      return structuredClone(s);
    }};
    const {default:BoardPanel}=await import("/src/dashboard/BoardButtonPanel.jsx");
    const {DEFAULT_VOICE_CONFIG,DEFAULT_BUTTON_ACTIONS}=await import("/src/DeviceDashboard.jsx");
    function BoardHarness({usb,wifi}){
      const [voice,setVoice]=React.useState(()=>({...DEFAULT_VOICE_CONFIG,buttonActions:DEFAULT_BUTTON_ACTIONS}));
      const caps=usb.connected ? usb.capabilities : wifi;
      return React.createElement(BoardPanel,{runtime:"esp-p4",voiceConfig:voice,buttonActions:voice.buttonActions,
        buttonValues:voice.buttonValues,voiceConfigOtaState:{},usbConnected:usb.connected,
        systemSettingsSupported:caps?.systemSettings===true,doublePressSupported:caps?.inputDoublePress===true,
        onVoiceConfigChange:patch=>setVoice(previous=>({...previous,...patch}))});
    }
    usbTest.capabilities.systemSettings=true;usbTest.capabilities.inputDoublePress=true;
    window.renderTarget=(usb,wifi)=>root.render(React.createElement(Keyboard,{usb,wifi},React.createElement(BoardHarness,{usb,wifi})));
    renderTarget(usbTest,wifiTest);
  });
  const sw3Long=page.locator("#button-action-p4_sw3_long");
  const sw3Double=page.locator("#button-action-p4_sw3_double");
  assert.equal(await sw3Long.inputValue(),"system_settings");
  assert.equal(await page.locator("#button-action-p4_sw3_short").inputValue(),"page_back");
  await sw3Double.selectOption("session_next");
  await sw3Long.selectOption("disabled");
  await page.getByRole("button",{name:/恢复默认.*打开系统设置/}).click();
  assert.equal(await sw3Long.inputValue(),"system_settings");
  assert.equal(await sw3Double.inputValue(),"session_next");
  console.log("SW3 short/back, long/settings, double remap and isolated restore confirmed");
  await page.getByRole("tab",{name:"蓝牙快捷键"}).click();
  console.log("Opened keyboard settings");
  const set=page.getByRole("button",{name:"保存快捷键"});
  const edit=page.getByRole("combobox",{name:"主键",exact:true});
  await edit.selectOption("4");
  await set.click();
  await page.getByText("快捷键已保存。",{exact:true}).waitFor();
  assert.deepEqual(await page.evaluate(()=>calls.filter(c=>c.slot!=null).map(c=>[c.cmd,c.boardDeviceId,c.connectionId,c.chord])),
    [["usb_bluetooth_keyboard_config","usb-target","7",4]]);
  console.log("USB exact slot confirmed");
  await page.getByRole("button",{name:"录入组合键"}).click();
  await page.keyboard.down("b"); await page.keyboard.down("Control"); await page.keyboard.up("b"); await page.keyboard.up("Control");
  await page.getByText("已录入 左 Ctrl + B，保存后生效。").waitFor();
  console.log("Captured modifier after main key");
  await page.evaluate(()=>states["usb-target"].keys[0]=6);
  await set.click();
  await page.getByText(/已在设备上更改/).waitFor();
  assert.equal(await page.evaluate(()=>calls.filter(c=>c.slot!=null).length),1);
  await page.getByRole("button",{name:"取消修改"}).click();
  await edit.selectOption("7");
  await page.evaluate(()=>window.dropEcho=true);
  await set.click();
  await page.getByText(/设备回读与本次修改不同/).waitFor();
  await page.getByRole("button",{name:"取消修改"}).click();
  await page.evaluate(()=>{window.dropEcho=false;renderTarget({connected:false},wifiTest);});
  await page.waitForFunction(() => document.querySelector('select[aria-label="主键"]')?.value === "0");
  await edit.selectOption("10");
  await set.click();
  await page.getByText("快捷键已保存。",{exact:true}).waitFor();
  console.log("WLAN save confirmed");
  const wifiWrite=await page.evaluate(()=>calls.find(c=>c.slot!=null && c.cmd.startsWith("lan_")));
  assert.equal(wifiWrite.expectedBootId,"b".repeat(16)); assert.equal(wifiWrite.chord,10);
  await edit.selectOption("11");
  await page.evaluate(()=>window.blockRead=true);
  await set.click();
  await page.waitForFunction(()=>!!window.finishRead);
  await page.evaluate(()=>renderTarget(usbTest,wifiTest));
  await page.waitForFunction(()=>document.querySelector('select[aria-label="主键"]')?.value==="6");
  await page.evaluate(()=>finishRead());
  await page.waitForFunction(()=>!document.querySelector('button[aria-busy="true"]'));
  assert.equal(await page.evaluate(()=>calls.filter(c=>c.slot!=null && c.cmd.startsWith("lan_")).length),1);
  // Earlier cases ran on devices without the optional capability. Exercise both
  // upgraded transports with competitors that write *after* the last preflight.
  for(const [route,board,old] of [["usb","usb-target",6],["lan","wifi-target",10]]) {
    await page.evaluate(({route,board})=>{
      states[board].conditionalUpdate="keyboard-cas-v1";
      renderTarget(route==="usb"?usbTest:{connected:false},wifiTest);
    },{route,board});
    await page.waitForFunction(v=>document.querySelector('select[aria-label="主键"]')?.value===String(v),old);
    const legacyCount=()=>page.evaluate(()=>calls.filter(c=>c.slot!=null || c.enabled!=null).length);
    const before=await legacyCount();
    await edit.selectOption("12");
    await page.evaluate(()=>window.interleave="other");
    await set.click();
    await page.getByText("快捷键已保存。",{exact:true}).waitFor();
    const writes=()=>page.evaluate(board=>calls.filter(c=>c.conditional && c.boardDeviceId===board),board);
    const first=(await writes())[0];
    assert.equal(first.cmd,route+"_bluetooth_keyboard_config");
    assert.equal(route==="usb"?first.connectionId:first.expectedBootId,route==="usb"?"7":"b".repeat(16));
    assert.deepEqual(first.conditional,{slot:0,setChord:12,expectedChord:old});
    assert.equal(await page.evaluate(b=>states[b].keys[3],board),19);
    await edit.selectOption("13");
    await page.evaluate(()=>window.interleave="slot");
    await set.click();
    await page.getByRole("alert").filter({hasText:"键盘设置已变化"}).waitFor();
    await page.getByRole("button",{name:"取消修改"}).waitFor();
    assert.equal(await edit.inputValue(),"13");
    assert.equal(await set.isDisabled(),true);
    await page.getByRole("button",{name:"取消修改"}).click();
    assert.equal(await edit.inputValue(),"20");
    const toggle=page.getByRole("switch",{name:"蓝牙模式"});
    await page.evaluate(()=>window.interleave="mode");
    await toggle.click();
    await page.getByRole("switch",{checked:true}).waitFor();
    await page.getByRole("alert").filter({hasText:"键盘设置已变化"}).waitFor();
    await toggle.click();
    await page.getByRole("switch",{checked:false}).waitFor();
    // A bridge/firmware that returns only a snapshot must not count as an ACK.
    await edit.selectOption("14");
    await page.evaluate(()=>window.ignoreConditional=true);
    await set.click();
    await page.getByRole("alert").filter({hasText:"未确认条件更新"}).waitFor();
    await page.getByRole("button",{name:"取消修改"}).click();
    await page.evaluate(()=>window.ignoreConditional=false);
    assert.deepEqual((await writes()).map(c=>c.conditional),[
      {slot:0,setChord:12,expectedChord:old},{slot:0,setChord:13,expectedChord:12},
      {setEnabled:true,expectedEnabled:false},{setEnabled:false,expectedEnabled:true},
      {slot:0,setChord:14,expectedChord:20},
    ]);
    assert.equal(await legacyCount(),before,"no unconditional fallback after conflict or ambiguous ACK");
    console.log("PASS",route,"scoped conditional slot/mode, post-preflight races, independent slot, missing ACK and zero legacy retries");
  }
  await page.evaluate(()=>renderTarget({connected:false},{...wifiTest,controlTrusted:false}));
  await page.getByText(/授权 Wi-Fi 控制/).waitFor();
  assert.equal(await page.getByRole("switch").count(),0);
  await page.getByRole("tab",{name:"按钮配置"}).click();
  assert.equal(await sw3Double.count(),0);
  assert.equal(await page.locator("#button-action-p4_sw3_short").inputValue(),"page_back");
  assert.deepEqual(errors,[]);
  console.log("PASS real React USB priority, key capture, same-slot conflict, mismatched device echo, paired WLAN save, target change during preflight, untrusted read-only.");
} finally {await browser.close();}
