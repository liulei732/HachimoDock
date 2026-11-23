// Use the actual Component Center and modal; the native seam and pet are fixtures.
import assert from "node:assert/strict";
import {chromium} from "playwright";

const base = process.env.PET_TEST_URL || "http://127.0.0.1:4173";
assert.equal(new URL(base).hostname, "127.0.0.1");
const browser = await chromium.launch({headless:true, executablePath:process.env.PET_TEST_BROWSER});
try {
  const page = await browser.newPage();
  const errors = [];
  page.setDefaultTimeout(6000);
  page.on("pageerror", error=>errors.push(error.message));
  await page.route("**/*", async route=>{
    const url = new URL(route.request().url());
    if (url.origin !== base) return route.abort();
    if (url.pathname === "/__edition_test") return route.fulfill({contentType:"text/html",body:'<div id="root"></div>'});
    if (url.pathname === "/src/shell/DeviceContext.jsx") {
      const response = await route.fetch();
      return route.fulfill({response,body:(await response.text())+"\nexport { DeviceContext as FixtureContext };\n"});
    }
    return route.continue();
  });
  await page.goto(base + "/__edition_test");
  await page.evaluate(async ()=>{
    const Refresh = (await import("/@react-refresh")).default;
    Refresh.injectIntoGlobalHook(window);
    window.$RefreshReg$=()=>{}; window.$RefreshSig$=()=>type=>type;
    window.__vite_plugin_react_preamble_installed__=true;
    localStorage.setItem("pet-manager.onboarding.component-center.v2","1");
    const calls = window.editionCalls = [];
    window.__TAURI_EVENT_PLUGIN_INTERNALS__={unregisterListener:()=>{}};
    window.__TAURI_INTERNALS__={transformCallback:()=>1, unregisterCallback:()=>{},
      invoke:async(command,args={})=>{
        calls.push({command,args});
        if (command==="list_component_library") return {components:[], libraryPath:""};
        if (command==="usb_get_status") return {connected:false};
        if (command==="import_component_edition") return {name:"实验包"};
        if (command.startsWith("plugin:") || command==="set_exit_reasons") return 0;
        return {};
      }};
    const React=(await import("/node_modules/.vite/deps/react.js")).default;
    const {createRoot}=(await import("/node_modules/.vite/deps/react-dom_client.js")).default;
    const {FixtureContext}=await import("/src/shell/DeviceContext.jsx");
    const {ToastProvider}=await import("/src/shell/ToastStack.jsx");
    const {default:Center}=await import("/src/ComponentCenter.jsx");
    const value={usb:{connected:false},currentDisplay:{appearance:{name:"团子"}}};
    createRoot(document.getElementById("root")).render(React.createElement(FixtureContext.Provider,{value},
      React.createElement(ToastProvider,null,React.createElement(Center,{active:true}))));
  });
  try {
    for (const [component,name,label] of [
      ["book-pocket","每天一本书","导入离线书袋"], ["hachimi-learn","每日新知","导入离线知识精选"],
      ["cozy-pop-stars","消星星","导入每日与续局版"], ["tomato-clock","番茄钟","导入当前宠物专注版"],
    ]) {
      await page.getByText(name,{exact:true}).first().click();
      await page.getByRole("button",{name:label,exact:true}).click();
      await page.waitForFunction(id=>window.editionCalls.some(c=>c.command==="import_component_edition" && c.args.component===id),component);
      await page.getByRole("button",{name:"关闭组件详情"}).click();
    }
    const calls = await page.evaluate(()=>window.editionCalls);
    assert.equal(calls.filter(c=>c.command==="import_component_edition").length,4);
    const focus = calls.find(c=>c.args.component==="tomato-clock" && c.command==="import_component_edition");
    assert.equal(focus.args.petName,"团子");
    assert.equal(focus.args.portrait,null);
    assert.ok(calls.filter(c=>c.command==="list_component_library").length>=5);
    assert.equal(calls.some(c=>/install|publish/.test(c.command)),false);
    assert.deepEqual(errors,[]);
  } catch (error) {
    console.error(errors, (await page.locator("body").innerText()).slice(-4000));
    throw error;
  }
  console.log("PASS four Component Center imports, selected pet, library refresh, no device sync");
} finally {
  await browser.close();
}
