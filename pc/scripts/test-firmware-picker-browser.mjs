// Real desktop routes and controls with in-memory Tauri replies. All browser traffic is intercepted.
import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";
import { build } from "esbuild";
import { chromium } from "playwright";

const root = fileURLToPath(new URL("../", import.meta.url));
const { outputFiles } = await build({
  absWorkingDir: root, jsx: "automatic", bundle: true, write: false, format: "iife", external: ["./ComponentCenter", "./CustomAvatarWizard", "./AppearanceGallery", "./AppearanceDetail", "virtual:p4-wasm-font"],
  loader: { ".css":"empty", ".wasm":"dataurl", ".svg":"dataurl", ".png":"dataurl", ".webp":"dataurl" },
  define: { "process.env.NODE_ENV":'"production"', "import.meta.env.DEV":"false", "import.meta.env.BASE_URL":'"/"' },
  stdin: { loader:"jsx", resolveDir:root, contents:
    'import React from "react";import {createRoot} from "react-dom/client";import App from "./src/App.jsx";const root=createRoot(document.querySelector("main"));let mount=0;window.restartUI=()=>root.render(<App key={++mount}/>);restartUI();' },
});
const browser = await chromium.launch({headless:true, executablePath:process.env.PET_TEST_BROWSER});
try {
  const page = await browser.newPage();
  page.setDefaultTimeout(5000);
  const errors = [];
  page.on("pageerror", e => { errors.push(e.message); console.error(e.message); });
  await page.route("**/*", r => r.request().url() === "https://fixture.test/"
    ? r.fulfill({contentType:"text/html",body:"<main></main>"}) : r.abort());
  await page.goto("https://fixture.test/");
  await page.evaluate(() => {
    const a = "p4-111111111111", b = "p4-222222222222";
    const device = (id, connectionId, bootId) => ({boardDeviceId:id,connected:true,connectionId,bootId,
      firmware:"1.0.0",runtime:"esp-p4",portName:"fixture",capabilities:{}});
    const f = window.fixture = {a,b,device,usb:device(a,"11","1111111111111111"),pickers:[],updates:[],
      bindings:[{boardDeviceId:a}], permissions:[], callbacks:[], listeners:new Map()};
    f.emit = (name,payload) => {
      for (const [id,event] of f.listeners) if (name === event) f.callbacks[id]({payload});
    };
    f.plug = (...args) => {
      f.usb = args.length ? device(...args) : {connected:false};
      f.emit(args.length?"usb-message":"usb-disconnected",{topic:"hello"});
    };
    window.__TAURI_EVENT_PLUGIN_INTERNALS__={unregisterListener:(_,id)=>f.listeners.delete(id)};
    window.__TAURI_INTERNALS__={
      transformCallback: callback => f.callbacks.push(callback)-1,
      convertFileSrc: () => "data:,",
      invoke: async (cmd,args) => {
        switch (cmd) {
          case "plugin:event|listen": f.listeners.set(args.handler,args.event);return args.handler;
          case "plugin:dialog|open": return new Promise((resolve,reject)=>f.pickers.push({resolve,reject}));
          case "usb_update_firmware": return new Promise((resolve,reject)=>f.updates.push({args,resolve,reject}));
          case "set_exit_reasons": f.permissions=args.reasons;return;
          case "load_generation_provider_configs": return {};
          case "load_device_bindings": return f.bindings;
          case "usb_get_status": return f.usb;
          case "lan_agent_discover": return [];
          case "lan_connection_snapshot": return f.pairing || {address:null,status:null};
          case "load_voice_chat_settings": case "load_device_asr_settings": return {};
          case "load_llm_network_settings": return {mode:"system",proxyUrl:"",certificateConfigured:false};
          case "detect_local_agents": return {agents:[]};
          case "realtime_chat_status": return {active:false};
          default: return null;
        }
      }
    };
  });
  await page.addScriptTag({content:outputFiles[0].text});
  const menu = page.getByTitle("更多操作");
  const dialog = page.getByRole("dialog",{name:"ESP32-P4 固件升级"});
  const choose = dialog.locator(".firmware-update-modal__picker");
  const start = dialog.getByRole("button",{name:"开始升级"});
  const open = async () => {
    await menu.click();
    assert.equal(await page.getByRole("menuitem",{name:"复制桌面设备 ID"}).count(),0);
    await page.getByRole("menuitem",{name:"从文件升级 ESP32-P4 固件"}).click();
    await dialog.waitFor();
  };
  const settlePicker = (index, value, fail=false) => page.evaluate(([i,v,fail])=>{
    fixture.pickers[i][fail?"reject":"resolve"](v);
  },[index,value,fail]);
  const plug = (...args)=>page.evaluate(args=>fixture.plug(...args),args);
  await open();
  await choose.click();
  await plug("p4-222222222222","12","2222222222222222");
  await dialog.getByText(/USB 已切换到另一台设备/).waitFor();
  await settlePicker(0,"/first/stale.bin");
  assert.equal(await start.isDisabled(),true);
  assert.equal(await dialog.getByText(/stale.bin/).count(),0);
  await dialog.getByRole("button",{name:"取消"}).click();
  await open();
  await choose.click();
  await dialog.getByRole("button",{name:"取消"}).click();
  await open();
  await choose.click();
  await settlePicker(1,"late error",true);
  assert.equal(await choose.isDisabled(),true);
  assert.equal(await dialog.getByText("late error").count(),0);
  await settlePicker(2,"/second/chosen.bin");
  await dialog.getByText("chosen.bin",{exact:true}).waitFor();
  console.log("PASS board swap, close/reopen, stale picker rejection and finally");

  await plug("p4-222222222222","13","3333333333333333");
  await choose.getByText("选择 firmware.bin").waitFor();
  assert.equal(await start.isDisabled(),true);
  await choose.click();
  await page.getByTitle("其它设置",{exact:true}).click();
  await dialog.waitFor({state:"detached"});
  await page.getByTitle("桌宠伙伴",{exact:true}).click();
  await open();
  await settlePicker(3,"/previous-tab/stale.bin");
  assert.equal(await start.isDisabled(),true);
  assert.equal(await page.evaluate(()=>fixture.updates.length),0);
  console.log("PASS same-board reconnect clears preflight selection; leaving/reentering route rejects old picker");

  await choose.click();
  await settlePicker(4,"/target/new.bin");
  await start.click();
  const args = await page.evaluate(()=>fixture.updates[0].args);
  assert.equal(args.expectedBoardDeviceId,"p4-222222222222");
  assert.equal(args.expectedConnectionId,"13");
  assert.equal(args.firmwarePath,"/target/new.bin");
  assert.ok(args.updateId);
  const progress = (id,stage,percent) => page.evaluate(([id,stage,percent])=>{
    fixture.emit("usb-firmware-update-progress",{updateId:id,stage,percent});
  },[id,stage,percent]);
  await progress(args.updateId,"upload",24);
  await dialog.getByText("24%").waitFor();
  await progress("other-operation","upload",99);
  assert.equal(await dialog.getByText("99%").count(),0);
  await plug();
  await dialog.locator(".firmware-update-modal__subtitle").getByText("未知",{exact:true}).waitFor();
  await progress(args.updateId,"reboot",100);
  await dialog.getByText(/正在等待设备重启/).waitFor();
  await choose.getByText("new.bin",{exact:true}).waitFor();
  assert.ok(await page.evaluate(()=>fixture.permissions.some(reason=>reason.includes("固件升级中"))));
  await plug(args.expectedBoardDeviceId,"14","4444444444444444");
  await dialog.locator(".firmware-update-modal__subtitle").getByText("1.0.0",{exact:true}).waitFor();
  await progress(args.updateId,"validate",100);
  await dialog.getByText(/正在确认新版本有效/).waitFor();
  assert.equal(await dialog.getByRole("button",{name:"关闭"}).isDisabled(),true);
  assert.equal(await page.evaluate(()=>fixture.updates.length),1);
  await page.evaluate(()=>fixture.updates[0].resolve({version:"2.0.0",targetPartition:"ota_1",imageState:"valid"}));
  await dialog.getByText("新固件已在设备上确认有效").waitFor();
  await dialog.getByRole("button",{name:"完成"}).click();
  await page.waitForFunction(()=>!fixture.permissions.some(reason=>reason.includes("固件升级中")));
  console.log("PASS same-board OTA reboot/reconnect preserves one native update through verified receipt");

  await open(); await choose.click(); await settlePicker(5,"/old/upload.bin");await start.click();
  await plug("p4-111111111111","15","5555555555555555");
  await dialog.getByText(/USB 已切换到另一台设备/).waitFor();
  await dialog.getByRole("button",{name:"关闭"}).click();await open();
  await dialog.getByText("正在等待上一次升级结束。").waitFor();
  await page.evaluate(()=>fixture.updates[1].reject("previous board error"));
  await choose.waitFor();
  assert.equal(await dialog.getByText("previous board error").count(),0);
  await dialog.getByRole("button",{name:"取消"}).click();
  console.log("PASS late update completion cannot write into a reopened dialog for another board");

  await page.getByTitle("其它设置",{exact:true}).click();
  await page.getByRole("heading",{name:"其它设置",exact:true}).waitFor();
  const models = page.getByTitle("模型配置",{exact:true});
  await models.click();
  const network = page.getByText("连接方式、代理与企业证书",{exact:true});
  await network.waitFor();
  assert.equal(await network.evaluate(el=>el.closest(".card").querySelector(".card__title")?.textContent), "云服务网络");
  await models.click();
  await page.getByRole("button",{name:"返回",exact:true}).click();
  await page.getByRole("heading",{name:"其它设置",exact:true}).waitFor();
  console.log("PASS cloud networking is separate from the chat model and repeated navigation retains the original return page");
  await page.evaluate(()=>{
    fixture.bindings=[];fixture.usb={connected:false};
    fixture.pairing={address:"192.0.2.5",status:{address:"192.0.2.5",boardDeviceId:fixture.a,
      bootId:"0000000000000001",wifiConnected:true,controlTrusted:true,panelControl:true}};
    window.restartUI();
  });
  await menu.click();
  await page.getByRole("menuitem",{name:"ESP32-P4 设备诊断"}).waitFor();
  for (const name of ["复制桌面设备 ID","解绑设备","从文件升级 ESP32-P4 固件"]) {
    assert.equal(await page.getByRole("menuitem",{name}).count(),0);
  }
  console.log("PASS LAN-only pairing shows diagnostics and omits inapplicable USB and desktop binding actions");
  assert.deepEqual(errors,[]);
} finally {
  await browser.close();
}
