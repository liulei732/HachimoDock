// Mount the production settings and provider with synthetic native IPC, entirely in memory.
import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";
import { build } from "esbuild";
import { chromium } from "playwright";

const root = fileURLToPath(new URL("../", import.meta.url));
const result = await build({
  absWorkingDir: root, bundle: true, write: false, platform: "browser",
  loader: { ".css": "empty" }, define: { "process.env.NODE_ENV": '"production"' },
  stdin: { resolveDir: root, loader: "jsx", contents: `
    import React from "react";
    import {createRoot} from "react-dom/client";
    import {DeviceContextProvider,useDeviceContext} from "./src/shell/DeviceContext.jsx";
    import Panel from "./src/dashboard/LanAgentPairingPanel.jsx";
    function Settings({active}) {
      const {usb,wifi,pairedStatus}=useDeviceContext();
      return <><output>{usb.connected?"usb":wifi?"wifi":"offline"}|{pairedStatus?.boardDeviceId}</output>
        <div hidden={!active}><Panel usb={usb} active={active}/></div></>;
    }
    const root=createRoot(document.getElementById("root"));
    window.renderSettings=active=>root.render(<DeviceContextProvider><Settings active={active}/></DeviceContextProvider>);
    renderSettings(true);` },
});
const browser = await chromium.launch({ headless: true, executablePath: process.env.PET_TEST_BROWSER });
try {
  const page = await browser.newPage();
  const errors = [];
  page.on("pageerror", e => errors.push(e.message));
  page.setDefaultTimeout(4000);
  await page.route("http://fixture.test/", r => r.fulfill({ contentType:"text/html",body:'<div id="root"></div>' }));
  await page.goto("http://fixture.test/");
  await page.clock.install({ time: new Date("2025-01-01T00:00:00Z") });
  await page.clock.pauseAt(new Date("2025-01-01T00:00:01Z"));
  await page.evaluate(() => {
    const a="p4-111111111111", b="p4-222222222222", boot="0123456789abcdef";
    const fixture = window.fixture = {
      a,b,boot,ip:"192.0.2.1",usb:{connected:false},
      status:{boardDeviceId:a,bootId:boot,address:"192.0.2.1",wifiConnected:true,
        rebootSupported:true,componentDelivery:true,controlTrusted:true},
      devices:[{id:b,ip:"192.0.2.2",ready:true,supportsAutomatic:true,paired:false}],
      calls:[], hold:{}, replies:{}, trustFails:false,
    };
    window.__TAURI_INTERNALS__ = {
      transformCallback: () => 1,
      invoke: async (cmd, args) => {
        fixture.calls.push(cmd);
        if (fixture.hold[cmd]) {
          fixture.hold[cmd]=false;
          return new Promise((resolve,reject)=>{fixture.replies[cmd]={resolve,reject};});
        }
        switch(cmd) {
          case "usb_get_status": return fixture.usb;
          case "lan_connection_snapshot": return {address:fixture.ip,status:fixture.status,error:fixture.readError};
          case "lan_agent_discover": return fixture.devices;
          case "lan_trust_usb":
            if (args?.target?.boardDeviceId !== fixture.b || args.target.bootId !== fixture.boot ||
                args.target.connectionId !== fixture.usb.connectionId) throw Error("incorrect trust target");
            if (fixture.trustFails) throw Error("授权未完成，请重试");
            fixture.status={...fixture.status,controlTrusted:true};return;
          case "lan_agent_pair_confirm":
            fixture.ip=args.ip;
            fixture.status={...fixture.status,address:args.ip,boardDeviceId:args.id,controlTrusted:false};
            return args.ip;
          case "lan_agent_forget": fixture.ip=null;fixture.status=null;return;
          case "detect_local_agents": return {agents:[]};
          case "realtime_chat_status": return {active:false};
          default: return null;
        }
      },
    };
  });
  await page.addScriptTag({content:result.outputFiles[0].text});
  const refresh = page.getByRole("button",{name:"刷新状态"});
  const rescan = page.getByRole("button",{name:"重新扫描"});
  const reboot = page.getByRole("button",{name:"无线重启"});
  const confirm = page.getByRole("button",{name:"确认配对"});
  const route = page.locator("output");
  const readRoute = async expected => {
    await page.waitForFunction(x=>document.querySelector("output")?.textContent===x,expected,{polling:20});
  };
  const focus = () => page.evaluate(()=>window.dispatchEvent(new Event("focus")));
  const calls = cmd => page.evaluate(c=>fixture.calls.filter(x=>x===c).length,cmd);
  const advance = n => page.clock.runFor(n);

  await readRoute("wifi|p4-111111111111");
  await page.getByText("设备维护与配对管理").click();
  await reboot.waitFor();
  await page.evaluate(()=>{fixture.status={...fixture.status,controlTrusted:false};});
  await refresh.click();
  await reboot.waitFor({state:"detached"});
  await page.evaluate(()=>{fixture.status={...fixture.status,controlTrusted:true};});
  await focus();
  await reboot.waitFor();
  console.log("PASS fresh permission gain and revocation control wireless actions");

  await page.evaluate(()=>{fixture.hold["plugin:dialog|open"]=true;});
  await page.getByRole("button",{name:"无线安装组件"}).click();
  await page.evaluate(()=>{fixture.status={...fixture.status,controlTrusted:false};});
  await focus();
  await reboot.waitFor({state:"detached"});
  await page.evaluate(()=>fixture.replies["plugin:dialog|open"].resolve("/fixture/package.hachiapp"));
  assert.equal(await calls("lan_component_preview"),0);
  assert.equal(await calls("lan_component_install"),0);
  await page.evaluate(()=>{fixture.status={...fixture.status,controlTrusted:true};});
  await focus();
  await reboot.waitFor();
  console.log("PASS permission loss during file selection prevents component preparation and install");

  await page.evaluate(()=>{fixture.hold.lan_reboot=true;});
  await reboot.click();
  await page.evaluate(()=>{fixture.status={...fixture.status,bootId:"9876543210fedcba"};});
  await focus();
  await page.evaluate(()=>fixture.replies.lan_reboot.resolve({observed:fixture.status,scheduled:true}));
  await page.getByText("设备已重启并重新上线。").waitFor();
  assert.equal(await calls("lan_reboot"),1);
  await page.evaluate(()=>{fixture.status={...fixture.status,bootId:fixture.boot};});
  await focus();
  console.log("PASS one reboot retains its confirmed result across the same board's boot change");

  await page.getByRole("radio").check();
  await confirm.waitFor();
  await page.evaluate(()=>{fixture.hold.lan_agent_discover=true;});
  await rescan.click();
  assert.equal(await refresh.isEnabled(),true);
  await refresh.click();
  await advance(8100);
  await page.getByText(/发现设备超时/).waitFor();
  assert.equal(await page.getByRole("radio").count(),0);
  assert.equal(await confirm.count(),0);
  await page.evaluate(()=>{fixture.hold.lan_agent_discover=true;});
  const lateScan = await page.evaluateHandle(()=>fixture.replies.lan_agent_discover);
  await rescan.click();
  await page.evaluate(()=>fixture.replies.lan_agent_discover.reject(new Error("请允许客户端访问本地网络后重试。")));
  await page.getByRole("alert").filter({hasText:"允许客户端访问本地网络"}).waitFor();
  await rescan.click();
  await page.getByRole("radio").waitFor();
  await page.evaluate(reply=>reply.resolve([]),lateScan);
  await lateScan.dispose();
  assert.equal(await page.getByRole("alert").filter({hasText:"允许客户端访问本地网络"}).count(),0);
  assert.equal(await page.getByRole("radio").count(),1);
  await page.getByRole("radio").check();
  await page.clock.fastForward(151000);
  await page.getByText(/扫描结果已过期/).waitFor();
  assert.equal(await confirm.count(),0);
  console.log("PASS rescan timeout clears old choices, retry ignores late reply, discovery expires");

  await rescan.click();
  await page.getByRole("radio").check();
  await page.evaluate(()=>{fixture.hold.lan_connection_snapshot=true;});
  await focus();
  await confirm.click();
  await readRoute("wifi|p4-222222222222");
  await page.evaluate(()=>fixture.replies.lan_connection_snapshot.resolve({
    address:"192.0.2.1",status:{...fixture.status,address:"192.0.2.1",boardDeviceId:fixture.a,controlTrusted:true}
  }));
  await reboot.waitFor({state:"detached"});
  assert.equal(await route.textContent(),"wifi|p4-222222222222");
  console.log("PASS confirmed device switch discards previous in-flight permission/status");

  await page.evaluate(()=>{fixture.usb={connected:true,connectionId:"11",bootId:fixture.boot,boardDeviceId:fixture.a};});
  await advance(3100);
  await readRoute("usb|p4-222222222222");
  await page.getByText(/USB 连接与 Wi-Fi 配对目标尚未匹配/).waitFor();
  assert.equal(await calls("lan_trust_usb"),0);
  await page.evaluate(()=>{fixture.trustFails=true;fixture.usb={...fixture.usb,boardDeviceId:fixture.b,connectionId:"12"};});
  await advance(3100);
  await page.getByRole("alert").filter({hasText:"授权未完成"}).waitFor();
  await advance(10500);
  assert.equal(await calls("lan_trust_usb"),1);
  await page.evaluate(()=>{fixture.trustFails=false;});
  await page.getByRole("button",{name:"通过 USB 授权无线控制"}).click();
  await page.getByText("此电脑已获无线控制授权。").waitFor();
  assert.equal(await calls("lan_trust_usb"),2);
  assert.equal(await reboot.count(),0);
  await page.getByText("授权未完成，请重试").waitFor({state:"detached"});
  await page.evaluate(()=>window.renderSettings(false));
  await readRoute("usb|");
  const idle = await calls("lan_connection_snapshot");
  await advance(15100);
  assert.equal(await calls("lan_connection_snapshot"),idle);
  await page.evaluate(()=>{fixture.usb={connected:false};window.renderSettings(true);});
  await advance(3100);
  await readRoute("wifi|p4-222222222222");
  await reboot.waitFor();
  console.log("PASS simultaneous USB/LAN, mismatched USB, wired authorization retry, hidden poll suspension, unplug recovery");

  await page.evaluate(()=>{fixture.status=null;fixture.readError="发现了候选地址，未取得可验证的认证回执";});
  await refresh.click();
  await page.getByText("发现了候选地址，未取得可验证的认证回执", {exact:true}).waitFor();
  await reboot.waitFor({state:"detached"});
  await page.getByRole("button",{name:"忘记本机配对"}).waitFor();
  await page.evaluate(()=>{fixture.readError=undefined;});
  console.log("PASS staged authentication failure keeps the pairing and removes privileged actions");

  await page.evaluate(()=>{fixture.hold.lan_connection_snapshot=true;fixture.status=null;});
  await refresh.click();
  await advance(16000);
  await reboot.waitFor({state:"detached"});
  assert.equal(await page.getByRole("button",{name:"正在读取…"}).isDisabled(),true);
  await advance(14100);
  await reboot.waitFor({state:"detached"});
  await refresh.waitFor();
  assert.equal(await refresh.isEnabled(),true);
  await page.evaluate(()=>{fixture.status={...fixture.usb,boardDeviceId:fixture.b,bootId:fixture.boot,
    wifiConnected:true,controlTrusted:true,address:fixture.ip,rebootSupported:true};});
  await refresh.click();
  await reboot.waitFor();
  await page.evaluate(()=>fixture.replies.lan_connection_snapshot.resolve({address:null,status:null}));
  await page.getByRole("button",{name:"忘记本机配对"}).click();
  await page.getByText(/尚未配对，从下方选择/).waitFor();
  assert.equal(await reboot.count(),0);
  assert.deepEqual(errors,[]);
  console.log("PASS stalled status drops controls, manual refresh recovers, forget clears current target");
} finally {
  await browser.close();
}
