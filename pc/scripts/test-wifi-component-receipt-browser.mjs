// Real component center; native reads and writes terminate in an in-memory fixture.
import { chromium } from "playwright";
import assert from "node:assert/strict";
const base = process.env.PET_TEST_URL || "http://127.0.0.1:4173";
assert.equal(new URL(base).hostname, "127.0.0.1");
const browser = await chromium.launch({headless:true,executablePath:process.env.PET_TEST_BROWSER});
try {
  const page = await browser.newPage();
  page.setDefaultTimeout(8000);
  const errors = [];
  page.on("pageerror", e=>errors.push(e.message));
  await page.route("**/*", async route=>{
    const url = new URL(route.request().url());
    if (url.origin !== base) return route.abort();
    if (url.pathname === "/__component_test") return route.fulfill({contentType:"text/html",body:'<div id="root"></div>'});
    if (url.pathname === "/src/shell/DeviceContext.jsx") {
      const res = await route.fetch();
      return route.fulfill({response:res,body:(await res.text())+"\nexport { DeviceContext as FixtureContext };\n"});
    }
    return route.continue();
  });
  await page.goto(base+"/__component_test");
  await page.evaluate(async()=>{
    const Refresh=(await import("/@react-refresh")).default;
    Refresh.injectIntoGlobalHook(window);
    window.$RefreshReg$=()=>{};window.$RefreshSig$=()=>type=>type;
    window.__vite_plugin_react_preamble_installed__=true;
    localStorage.setItem("pet-manager.onboarding.component-center.v2","1");
    const entry={id:"fixture-wifi",name:"无线核对样例",version:"2.0.0",kind:"tool",path:"/fixture/component",versionHash:"fixture-hash",valid:true};
    const calls=window.calls=[];
    let committed=false;
    window.__TAURI_INTERNALS__={transformCallback:()=>1,unregisterCallback:()=>{},invoke:async(command,args={})=>{
      calls.push({command,args});
      if (command==="list_component_library") return {components:[entry],libraryPath:""};
      if (command==="usb_get_status") return {connected:false};
      if (command==="lan_panel_control" && args.operation==="readComponents") return {complete:true,items:committed?[entry]:[],supportsMultiple:true,maxInstalled:20};
      if (command==="lan_panel_control") return {};
      if (command==="inspect_clawpkg") return entry;
      if (command==="lan_component_preview") return {id:entry.id,version:entry.version,digest:"digest"};
      if (command==="lan_component_install") {committed=true;throw Error("无线安装提交结果未确认，请查看设备组件列表。");}
      if (command.startsWith("plugin:") || command==="set_exit_reasons") return 0;
      return {};
    }};
    const React=(await import("/node_modules/.vite/deps/react.js")).default;
    const {createRoot}=(await import("/node_modules/.vite/deps/react-dom_client.js")).default;
    const {FixtureContext}=await import("/src/shell/DeviceContext.jsx");
    const {ToastProvider}=await import("/src/shell/ToastStack.jsx");
    const {default:Center}=await import("/src/ComponentCenter.jsx");
    const value={usb:{connected:false},wifi:{boardDeviceId:"test-board",bootId:"0000000000000001",componentDelivery:true,controlTrusted:true,panelControl:true}};
    createRoot(document.getElementById("root")).render(React.createElement(FixtureContext.Provider,{value},React.createElement(ToastProvider,null,React.createElement(Center,{active:true}))));
  });
  try {
    await page.getByText("无线核对样例",{exact:true}).first().click();
    await page.getByRole("button",{name:"同步到设备",exact:true}).click();
    await page.getByText("安装结果待核对",{exact:true}).first().waitFor();
    assert.equal(await page.getByRole("button",{name:"重试",exact:true}).count(),0);
    const count=()=>page.evaluate(()=>window.calls.filter(c=>c.command==="lan_panel_control" && c.args.operation==="readComponents").length);
    const before=await count();
    await page.getByRole("button",{name:"刷新设备组件清单"}).click();
    await page.getByText("已从原设备的最新清单确认安装版本 2.0.0。").waitFor();
    assert.equal(await count(),before+1);
    assert.equal(await page.evaluate(()=>window.calls.filter(c=>c.command==="lan_component_install").length),1);
    assert.deepEqual(errors,[]);
  } catch(error) {
    console.error(errors,(await page.locator("body").innerText()).slice(-1600),await page.evaluate(()=>window.calls?.slice(-8)));
    throw error;
  }
  console.log("PASS Wi-Fi commit reply lost: normal component install, original boot inventory read, verified target version, one install, no retry.");
} finally {await browser.close();}
