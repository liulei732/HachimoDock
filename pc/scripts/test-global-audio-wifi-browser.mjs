// Both production entry points, using in-memory Tauri files and manually released device replies.
import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";
import { build } from "esbuild";
import { chromium } from "playwright";
import { encodeMonoPcm16Wav } from "../src/lib/audio-cue.js";

const root = fileURLToPath(new URL("../", import.meta.url));
const bundle = await build({
  absWorkingDir:root, bundle:true, write:false, format:"iife",
  loader:{".css":"empty",".png":"dataurl",".webp":"dataurl",".svg":"dataurl"},
  define:{"process.env.NODE_ENV":'"production"',"import.meta.env.DEV":"false","import.meta.env.BASE_URL":'"/"'},
  stdin:{resolveDir:root,loader:"jsx",contents:`
    import React from "react";
    import { createRoot } from "react-dom/client";
    import DeviceVolumePanel from "./src/dashboard/DeviceVolumePanel.jsx";
    import AppearanceDetail from "./src/AppearanceDetail.jsx";
    import { DeviceContext } from "./src/shell/DeviceContext.jsx";
    import { pairedWifiTarget } from "./src/shell/DeviceContext.pure.js";
    const root=createRoot(document.querySelector("main"));
    window.show=({usb, status, mount=0, boundId=usb?.boardDeviceId || ""})=>{
      const wifi=pairedWifiTarget(status);
      root.render(<DeviceContext.Provider value={{usb,wifi}} key={mount}>
        <div id="volume"><DeviceVolumePanel usb={usb} wifi={wifi} active={false}/></div>
        <div id="detail"><AppearanceDetail appearanceId="builtin-terrier" boardDeviceId={boundId}
          wifiConnected={Boolean(wifi)}/></div>
      </DeviceContext.Provider>);
    };`},
});
const browser = await chromium.launch({headless:true,executablePath:process.env.PET_TEST_BROWSER});
try {
  const page = await browser.newPage();
  page.setDefaultTimeout(5000);
  const errors=[];
  page.on("pageerror", e=>errors.push(e.message));
  await page.route("**/*",route=>route.request().url()==="https://fixture.test/"
    ? route.fulfill({contentType:"text/html",body:"<main></main>"}) : route.abort());
  await page.addInitScript(()=>{
    const files=new Map(JSON.parse(sessionStorage.getItem("fixture-files") || "[]"));
    addEventListener("pagehide",()=>sessionStorage.setItem("fixture-files",JSON.stringify([...files])));
    window.transfers=[];
    window.__TAURI_INTERNALS__={convertFileSrc:()=> "data:,",transformCallback:()=>1,
      invoke:async (cmd,args,opts)=>{
        switch(cmd) {
          case "usb_sync_appearance": case "lan_sync_global_audio_cues":
            return new Promise((resolve,reject)=>window.transfers.push({cmd,args,resolve,reject}));
          case "plugin:fs|exists": return files.has(args.path);
          case "plugin:fs|write_file": case "plugin:fs|write_text_file":
            files.set(decodeURIComponent(opts.headers.path),Array.from(args)); return;
          case "plugin:fs|rename":
            assertFile(args.oldPath);files.set(args.newPath,files.get(args.oldPath));files.delete(args.oldPath);return;
          case "plugin:fs|read_text_file": case "plugin:fs|read_file": assertFile(args.path);return files.get(args.path);
          case "plugin:fs|remove": files.delete(args.path);return;
          case "plugin:path|resolve_directory": return "/fixture";
          default: return null;
        }
      }};
    function assertFile(path){if(!files.has(path))throw Error("Missing fixture file "+path);}
  });
  await page.goto("https://fixture.test/");
  await page.addScriptTag({content:bundle.outputFiles[0].text});
  const panel=id=>page.locator("#"+id+" .detail-audio-config");
  const volume=panel("volume"), detail=panel("detail");
  const send=p=>p.getByRole("button",{name:"下发音效",exact:true});
  const pending=p=>p.getByText("当前设备尚未确认本版提示音。",{exact:true}).waitFor();
  const confirmed=p=>p.getByText("当前设备已确认本版全局提示音。",{exact:true}).waitFor();
  const a={boardDeviceId:"p4-000000000001",bootId:"0000000000000001",wifiConnected:true,controlTrusted:true,globalAudioCues:true};
  const b={...a,boardDeviceId:"p4-000000000002",bootId:"0000000000000002"};
  const usb={connected:true,boardDeviceId:b.boardDeviceId,bootId:b.bootId,connectionId:"22"};
  const show=async options=>{
    await page.evaluate(options=>window.show(options),options);
    await page.evaluate(()=>new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))));
  };
  let count=0;
  async function start(p,cmd,args){
    await send(p).click();
    await page.waitForFunction(n=>transfers.length===n,++count);
    const index=count-1;
    assert.deepEqual(await page.evaluate(i=>({cmd:transfers[i].cmd,args:transfers[i].args}),index),{cmd,args});
    return async (result={ok:true})=>page.evaluate(([i,r])=>{
      if(typeof r==="string")transfers[i].reject(r);else transfers[i].resolve(r);
    },[index,result]);
  }
  const lan=(p,t)=>start(p,"lan_sync_global_audio_cues",{boardDeviceId:t.boardDeviceId,bootId:t.bootId});
  await show({status:{...a,globalAudioCues:undefined,appearanceCapacity:8388608}});
  for(const p of [volume,detail]){
    await p.getByText(/当前固件下发全局提示音需要 USB/).waitFor();
    assert(await send(p).isDisabled());assert(await p.locator("input[type=file]").isEnabled());
  }
  await show({status:{...a,controlTrusted:false}});
  await volume.getByText(/授权 Wi-Fi 控制/).waitFor();assert(await send(detail).isDisabled());
  assert.equal(await page.evaluate(()=>transfers.length),0);
  console.log("PASS  old firmware and untrusted Wi-Fi keep local edits available and issue no transfer");

  await show({status:a});
  const oldA=await lan(detail,a);
  await show({status:b});
  const currentB=await lan(volume,b);
  await oldA();
  await pending(detail);
  assert(await volume.getByRole("button",{name:"处理中…"}).isDisabled());
  await currentB();
  await Promise.all([confirmed(volume),confirmed(detail)]);
  await show({status:a});await pending(volume);
  console.log("PASS  both entry points pin board and boot; late board A success cannot settle board B");

  const older=await lan(volume,a);
  const bytes=Buffer.from(encodeMonoPcm16Wav(new Float32Array(800)));
  await detail.locator("input[type=file]").setInputFiles({name:"cue.wav",mimeType:"audio/wav",buffer:bytes});
  await detail.getByRole("button",{name:"恢复默认"}).waitFor();
  await older();await send(volume).waitFor();await pending(volume);
  const failed=await lan(detail,a);await failed({ok:false,error:"设备未确认"});
  await detail.getByRole("alert").getByText("设备未确认",{exact:true}).waitFor();await pending(detail);
  const success=await lan(detail,a);await success();await confirmed(volume);
  await show({status:b});await pending(detail);
  console.log("PASS  real WAV edit invalidates both boards; old revision and failed receipt stay pending; retry succeeds");

  await show({usb,status:a});
  const wired=await start(volume,"usb_sync_appearance",{boardDeviceId:b.boardDeviceId,appearanceId:"global-audio-cues",audioOnly:true});
  await wired();await confirmed(detail);
  await show({usb,status:a,boundId:""});assert(await send(detail).isDisabled());
  await show({status:b,mount:1});await confirmed(detail);
  await page.reload();await page.addScriptTag({content:bundle.outputFiles[0].text});
  await show({status:a});await confirmed(volume);
  console.log("PASS  simultaneous connections use USB only; binding guard, cross-transport remount and persisted receipt");

  const c={...a,boardDeviceId:"p4-000000000003"};
  await show({status:c});
  count=0; // New native mock after page reload.
  const beforeReboot=await lan(volume,c);
  const reboot={...c,bootId:"0000000000000003"};
  await show({status:reboot});await beforeReboot();
  await pending(volume);
  const beforeRevoke=await lan(detail,reboot);
  await show({status:{...reboot,controlTrusted:false}});
  await show({status:reboot});await beforeRevoke("stale failure");
  assert.equal(await detail.getByRole("alert").count(),0);await pending(detail);
  const beforeExit=await lan(volume,reboot);
  await show({status:reboot,mount:2});await beforeExit();await pending(detail);
  const afterExit=await lan(volume,reboot);
  await show({status:{...reboot,clockSynchronized:true},mount:2});
  await afterExit();await confirmed(volume);await confirmed(detail);
  console.log("PASS  reboot, revoke/re-authorize and exit/re-entry discard stale success/error; unrelated polling preserves request");

  assert.deepEqual(errors,[]);
} finally {await browser.close();}
