// Production React pages, native IPC fixture, Playwright virtual time.
import assert from "node:assert/strict";
import {createHash} from "node:crypto";
import {readFile, writeFile} from "node:fs/promises";
import {chromium} from "playwright";

const base = process.env.PET_TEST_URL || "http://127.0.0.1:4173";
assert.equal(new URL(base).hostname, "127.0.0.1");
const output = process.argv[2];
assert.ok(output, "Supply an output JSON path");
const duration = Number(process.env.TRAY_WINDOW_MS || 1_800_000);
const browser = await chromium.launch({headless:true, executablePath:process.env.PET_TEST_BROWSER});
const scenarios = ["usb-dashboard", "unplugged-smart-home", "usb-keyboard"];
const management = ["local_bridge_request /agent/status", "ensure_device_voice_runtime", "smart_home_request status", "usb_bluetooth_keyboard_config"];
const heartbeat = ["usb_get_status","lan_connection_snapshot"];
const requiredListeners = ["usb-message","usb-disconnected","realtime-chat-request","lan-realtime-press"];

async function measure(scenario, hidden) {
  const page = await browser.newPage();
  const errors=[];
  page.on("pageerror", err=>errors.push(err.message));
  try {
    await page.route("**/*", route=>{
      const url = new URL(route.request().url());
      if (url.origin!==base) return route.abort();
      if (url.pathname==="/__tray_test") return route.fulfill({contentType:"text/html",body:'<div id="root"></div>'});
      return route.continue();
    });
    await page.goto(base+"/__tray_test");
    const start=new Date("2026-01-07T12:00:00Z");
    await page.clock.install({time:start});
    await page.clock.pauseAt(new Date(+start+1000));
    await page.evaluate(async({scenario})=>{
      const Refresh=(await import("/@react-refresh")).default;
      Refresh.injectIntoGlobalHook(window);
      window.$RefreshReg$=()=>{}; window.$RefreshSig$=()=>type=>type;
      window.__vite_plugin_react_preamble_installed__=true;
      const board="p4-123456789abc", boot="0000000000000001";
      const callbacks=new Map(), subscriptions=new Map();
      let id=0, state="visible", counts={}, first={}, started=Date.now(), bridgeDown=false;
      Object.defineProperties(document,{
        visibilityState:{configurable:true,get:()=>state}, hidden:{configurable:true,get:()=>state==="hidden"},
      });
      window.trayBench={
        visibility(value) {state=value;document.dispatchEvent(new Event("visibilitychange"));},
        reset(){counts={};first={};started=Date.now();},
        snapshot(){return {counts,first,subscriptions:[...subscriptions.values()].map(s=>s.event).sort()};},
        breakBridge(){bridgeDown=true;},
        bridgeDown(){return bridgeDown;},
        async usbHint(){
          for(const s of subscriptions.values()) if(s.event==="usb-message")
            await callbacks.get(s.handler)({payload:{topic:"availability"}});
        },
        async press(){
          for(const s of subscriptions.values()) if(s.event==="lan-realtime-press")
            await callbacks.get(s.handler)({payload:{boardDeviceId:board,bootId:boot,seq:11}});
        },
      };
      window.__TAURI_EVENT_PLUGIN_INTERNALS__={unregisterListener:(_event,id)=>subscriptions.delete(id)};
      window.__TAURI_INTERNALS__={
        transformCallback(fn){callbacks.set(++id,fn);return id;},
        unregisterCallback(id){callbacks.delete(id);},
        invoke:async(command,args={})=>{
          let key=command;
          if(command==="local_bridge_request") key+=" "+args.route.split("?")[0];
          if(command==="smart_home_request") key+=" "+args.operation;
          counts[key]=(counts[key]||0)+1;
          first[key]??=Date.now()-started;
          switch(command) {
            case "plugin:event|listen": subscriptions.set(++id,args);return id;
            case "usb_get_status":return scenario.startsWith("usb-")
              ? {connected:true,runtime:"esp-p4",boardDeviceId:board,bootId:boot,connectionId:"1",capabilities:{bluetoothKeyboardConfig:"p4-ble-keyboard-config-v1"}}
              : {connected:false};
            case "lan_connection_snapshot":return {address:false,status:null};
            case "realtime_chat_status":return {active:false,state:"idle"};
            case "lan_realtime_toggle":return {active:false};
            case "usb_bluetooth_keyboard_config":return {ok:true,enabled:false,boardDeviceId:board,keys:Array(20).fill(0)};
            case "smart_home_request":return {connected:false,devices:[],scenes:[],pending:[],tasks:[],records:[]};
            case "load_device_bindings":return [{boardDeviceId:board}];
            case "list_appearances":return [];
            case "detect_local_agents":return {agents:[]};
            case "ensure_bridge_runtime":bridgeDown=false;return {running:true};
            case "local_bridge_request":
              if(bridgeDown) throw Error("fixture Bridge offline");
              return {ok:true,agents:[],sessions:[],events:[],cursor:0};
            case "ensure_device_voice_runtime":return {running:false,deferred:true};
            default:return {};
          }
        },
      };
      const {mount}=await import("/scripts/tray-benchmark-fixture.jsx");
      mount(scenario,board);
    },{scenario});
    await page.waitForFunction(()=>window.trayBench.snapshot().subscriptions.includes("lan-realtime-press"),null,{polling:50,timeout:5000});
    if(scenario==="usb-keyboard") {
      // A DOM click runs the same React tab handler without advancing layout timers.
      await page.getByRole("tab",{name:"蓝牙快捷键"}).evaluate(tab=>tab.click());
    }
    await page.clock.runFor(10_000);
    await page.evaluate(hidden=>{
      if(hidden) window.trayBench.visibility("hidden");
      window.trayBench.reset();
    },hidden);
    await page.clock.runFor(duration); // run every due timer, including repeating heartbeats
    const steady=await page.evaluate(()=>window.trayBench.snapshot());
    assert.deepEqual(errors,[]);
    for(const name of requiredListeners) assert.ok(steady.subscriptions.includes(name),name);
    assert.ok(steady.counts.usb_get_status>0);
    if(scenario==="unplugged-smart-home") assert.ok(steady.counts.lan_connection_snapshot>0);
    // Separate windows keep probe and restoration calls out of the idle totals.
    const presses=await page.evaluate(async()=>{
      window.trayBench.reset();await window.trayBench.press();return window.trayBench.snapshot().counts.lan_realtime_toggle||0;
    });
    assert.equal(presses,1);
    await page.evaluate(()=>window.trayBench.usbHint());
    assert.ok(await page.evaluate(()=>window.trayBench.snapshot().counts.usb_get_status>0));
    let restore=null, bridgeRecovery=null;
    if(hidden){
      await page.evaluate(()=>{window.trayBench.reset();window.trayBench.visibility("visible");});
      await page.clock.runFor(5000);
      restore=await page.evaluate(()=>window.trayBench.snapshot().first);
      await page.evaluate(()=>{window.trayBench.visibility("hidden");window.trayBench.reset();window.trayBench.breakBridge();});
      await page.clock.runFor(15_000);
      bridgeRecovery=await page.evaluate(()=>({
        restored:!window.trayBench.bridgeDown(),
        firstEnsureVirtualMs:window.trayBench.snapshot().first.ensure_bridge_runtime,
      }));
      assert.ok(bridgeRecovery.restored && bridgeRecovery.firstEnsureVirtualMs<=10_000);
    }
    const counts=steady.counts;
    const sum=names=>names.reduce((n,key)=>n+(counts[key]||0),0);
    return {scenario,visibility:hidden?"hidden":"visible",virtualMs:duration,counts,
      managementCalls:sum(management),heartbeatCalls:sum(heartbeat),totalInvokes:sum(Object.keys(counts)),
      registeredKeyAndTransportListeners:requiredListeners,acceptedInjectedKeyPresses:presses,
      injectedUsbAvailabilityTriggeredRead:true, hiddenBridgeRecoveryProbe:bridgeRecovery,
      restoreFirstInvokeVirtualMs:restore};
  } catch(error) {
    throw new Error(scenario+" "+(hidden?"hidden":"visible")+": "+[error.message,...errors].join(" | "),{cause:error});
  } finally {await page.close();}
}
try {
  const samples=await Promise.all(scenarios.flatMap(s=>[false,true].map(h=>measure(s,h))));
  if (process.argv.includes("--check")) {
    for (const scenario of scenarios) {
      const [shown,tray]=samples.filter(s=>s.scenario===scenario);
      assert.ok(shown.managementCalls>0);
      assert.equal(tray.managementCalls,0);
      assert.equal(tray.heartbeatCalls,shown.heartbeatCalls);
      for (const key of ["local_bridge_request /agent/session-events","local_bridge_request /agent/sessions"]) {
        assert.ok(shown.counts[key]>0);
        assert.equal(tray.counts[key],shown.counts[key]);
      }
      const expectedRefresh=scenario==="usb-dashboard" ? management.slice(0,2) : [scenario==="usb-keyboard" ? "usb_bluetooth_keyboard_config" : "smart_home_request status"];
      for (const key of expectedRefresh) assert.equal(tray.restoreFirstInvokeVirtualMs[key],0);
    }
  }
  const sourceSha256={};
  for(const name of ["DeviceDashboard.jsx","dashboard/useAgentSessionFeed.js","shell/DeviceContext.jsx","SmartHome.jsx","lib/visible-poll.js"]){
    sourceSha256[name]=createHash("sha256").update(await readFile(new URL("../src/"+name,import.meta.url))).digest("hex");
  }
  await writeFile(output,JSON.stringify({browser:browser.version(),durationMs:duration,management,heartbeat,sourceSha256,samples},null,2)+"\n");
} finally {await browser.close();}
