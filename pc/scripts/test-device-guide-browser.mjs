// Exercise the production guide in Chromium. Only the native transport is simulated.
import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";
import { build } from "esbuild";
import { chromium } from "playwright";

const root = fileURLToPath(new URL("../", import.meta.url));
const bundle = await build({
  bundle: true, write: false, format: "iife", jsx: "automatic",
  define: { "process.env.NODE_ENV": '"production"' },
  stdin: { resolveDir: root, loader: "jsx", contents: `
    import React, {useState} from "react";
    import {createRoot} from "react-dom/client";
    import DeviceGuideModal from "./src/DeviceGuideModal.jsx";
    function App(){
      const [open, setOpen] = useState(true);
      const [target, setTarget] = useState(window.initial);
      window.target = setTarget;
      window.openGuide = () => setOpen(true);
      return <DeviceGuideModal {...target} isOpen={open} setGuideOpen={setOpen} onClose={()=>setOpen(false)}/>;
    }
    createRoot(document.querySelector("main")).render(<App/>);` },
});
const browser = await chromium.launch({ executablePath: process.env.PET_TEST_BROWSER });
try {
  const page = await browser.newPage();
  const errors = [];
  page.on("pageerror", e => errors.push(e.message));
  page.setDefaultTimeout(4000);
  await page.route("**/*", route => route.request().url() === "https://guide.test/"
    ? route.fulfill({ contentType: "text/html", body: "<main></main>" }) : route.abort());
  await page.goto("https://guide.test/");
  const board = "p4-111111111111", boot = "1111111111111111";
  const usb = { connected: true, boardDeviceId: board, bootId: boot, connectionId: "1" };
  const wifi = { wifiConnected: true, panelControl: true, controlTrusted: true, boardDeviceId: board, bootId: boot };
  await page.evaluate(({usb, board, boot}) => {
    window.initial = { runtime: "esp-p4", usb };
    window.f = { calls: [], page: "main", active: "snake-turn", board, boot };
    window.__TAURI_INTERNALS__ = { invoke: async (command, args) => {
      f.calls.push({command, args});
      if (f.defer) {
        f.defer = false;
        await new Promise(resolve => { f.resume = resolve; });
      }
      if (f.fail) throw Error("fixture");
      if (command === "usb_send_speech") return null;
      const base = {ok:true, boardDeviceId:f.board, bootId:f.boot};
      if (command === "usb_get_diagnostics" || args.operation === "readDiagnostics")
        return {...base, runtime:{screenPage:f.page, miniappActive:f.running ?? true}};
      if (command === "list_device_widgets" || args.operation === "readComponents")
        return {...base, activeWidgetId:f.active, selectedWidgetId:"snake-turn",
          warnings:[], freshness:"live", complete:true, items:[{id:f.active,active:true,manifestState:"valid"}]};
      throw Error("unexpected native command");
    }};
  }, {usb, board, boot});
  await page.addScriptTag({content:bundle.outputFiles[0].text});
  const button = name => page.getByRole("button",{name, exact:true});
  const seen = text => page.getByText(text,{exact:true}).waitFor();
  const progress = n => seen(`已体验 ${n}/3 项`);
  const read = () => button("读取设备页面").click();
  const set = values => page.evaluate(values => Object.assign(f,values),values);

  await progress(1);
  await button("发送测试消息").click();
  await seen("测试消息已发送，等待确认屏幕反馈。");
  await progress(1);
  await read();
  await seen("设备当前页面：桌宠主屏");
  await set({page:"components"});
  await read();
  await seen("设备回读：桌宠主屏 → 组件菜单");
  await progress(2);
  await set({page:"app", running:false});
  await read();
  await seen("设备已进入组件页面，尚未报告有组件在运行。");
  await progress(2);
  await set({running:true, active:"city-radio"});
  await read();
  await seen("设备已进入组件页面，尚未核对到本次体验的离线组件。");
  await progress(2);
  await set({active:"snake-turn"});
  await read();
  await seen("设备回读：像素贪吃蛇已打开");
  await progress(3);
  await button("操作教程").click();
  await seen("用摇杆选择，用 SW2 切换页面");
  await button("上一页").click();
  await progress(3);

  await set({defer:true});
  await read();
  await page.waitForFunction(()=>typeof f.resume === "function");
  await page.evaluate(()=>target({runtime:"esp-p4",usb:{connected:false}}));
  await progress(0);
  await page.evaluate(()=>f.resume());
  assert.equal(await button("发送测试消息").isDisabled(),true);
  await progress(0);
  await button("完成").click();

  // Wi-Fi-only first visit and re-entry. A wrong-boot read never earns credit.
  await page.evaluate(wifi=>{
    localStorage.clear();
    f.calls=[];
    target({runtime:"esp-p4",wifi,wifiEntry:true});
  },wifi);
  await progress(1);
  assert.equal(await button("发送测试消息").count(),0);
  await set({boot:"2222222222222222",page:"app"});
  await read();
  await seen("这次未能核对设备状态。可以重试，也可以按屏幕实际情况手动确认。");
  await progress(1);
  await set({boot, page:"main"});
  await read();
  await seen("设备当前页面：桌宠主屏");
  await set({page:"app",active:"wooden-fish"});
  await read();
  await seen("设备回读：静心木鱼已打开");
  await progress(3);
  const calls = await page.evaluate(()=>f.calls);
  assert.deepEqual(calls.map(c=>c.args.operation),["readDiagnostics","readDiagnostics","readDiagnostics","readComponents"]);
  assert(calls.every(c=>c.command==="lan_panel_control" && c.args.boardDeviceId===board && c.args.expectedBootId===boot));
  await button("完成").click();
  await button("首次设备体验").click();
  await progress(1);

  await page.evaluate(wifi=>target({runtime:"esp-p4",wifi:{...wifi,panelControl:false},wifiEntry:true}),wifi);
  await seen("当前连接未提供页面回读，手动确认可跳过。");
  assert.equal(await button("读取设备页面").count(),0);
  await button("完成").click();

  // Legacy USB guide is still available with all three tutorial tabs.
  await page.evaluate(usb=>{target({runtime:"linux",usb});openGuide();},usb);
  await progress(1);
  await set({fail:true});
  await button("发送测试消息").click();
  await seen("测试消息发送失败，可以重试或直接在设备上切屏。");
  await progress(1);
  await button("我看到了").click();
  await progress(2);
  await button("我已打开离线组件").click();
  await progress(3);
  await button("操作教程").click();
  assert.equal(await page.getByRole("tab").count(),3);
  await seen("短按屏幕前红色编码旋钮 = 切到另一页");
  await page.getByRole("tab",{name:/组件按键/}).click();
  await seen("Token 消耗");
  await button("完成").click();
  assert.deepEqual(errors,[]);
  console.log("PASS USB send/readback, paired Wi-Fi, offline app identity, stale replies, optional confirmations and legacy tutorial");
} finally {
  await browser.close();
}
