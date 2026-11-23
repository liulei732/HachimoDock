// Exercise the shipped runtime with synthetic native data. No device or account needed.
import assert from "node:assert/strict";
import { mkdir, readFile } from "node:fs/promises";
import { chromium } from "playwright";

const base = process.env.PET_TEST_URL || "http://127.0.0.1:4175";
assert.equal(new URL(base).hostname, "127.0.0.1");
async function fixture(id) {
  const directory = new URL(`../builtin-clawpkgs/${id}/`, import.meta.url);
  const appRuntime = JSON.parse(await readFile(new URL("runtime/widget.json", directory)));
  const defaultBindings = JSON.parse(await readFile(new URL("buttons.json", directory)));
  const sceneSprites = (appRuntime.surface.images || []).map(image => ({
    ...image, dataUrl: new URL(image.asset, base + "/builtin-clawpkgs/" + id + "/").href,
  }));
  return { id, appRuntime, defaultBindings, sceneSprites };
}
async function captureCover(page, id) {
  if (!process.env.PET_TEST_COVERS) return;
  const directory = new URL(`../builtin-clawpkgs/${id}/pc/`, import.meta.url);
  await mkdir(directory, { recursive: true });
  await page.getByRole("img", { name: /设备预览$/ }).screenshot({ path: new URL("cover.png", directory).pathname });
}
const browser = await chromium.launch({ executablePath: process.env.PET_TEST_CHROMIUM || undefined });
try {
  const page = await browser.newPage();
  page.setDefaultTimeout(8000);
  const errors = [];
  page.on("pageerror", error => errors.push(error.message));
  await page.route("**/__computer", route => route.fulfill({
    contentType: "text/html",
    body: '<main id="root" style="width:640px"></main>',
  }));
  await page.goto(base + "/__computer");
  await page.evaluate(async component => {
    const refresh = (await import("/@react-refresh")).default;
    refresh.injectIntoGlobalHook(window);
    window.$RefreshReg$ = () => {};
    window.$RefreshSig$ = () => type => type;
    window.__vite_plugin_react_preamble_installed__ = true;
    const metrics = [["CPU", "18%"], ["内存", "42%"], ["网络接收", "67 KiB/s"], ["网络发送", "8 KiB/s"],
      ["开机时长", "2h 12m"], ["可用内存", "9.2 GiB"], ["交换空间", "0.5 GiB"], ["CPU 峰值", "52%"]];
    window.__TAURI_EVENT_PLUGIN_INTERNALS__ = { unregisterListener() {} };
    window.__TAURI_INTERNALS__ = { invoke: async (command, args) => {
      if (command === "plugin:event|listen" || command === "plugin:event|unlisten") return 1;
      if (command !== "local_widget_snapshot") throw Error(command);
      if (args.source === "stocks.watchlist") return { status: "ok", date: "2026-11-21",
        message: "默认名单示例",
        rows: ["小米集团", "阿里巴巴", "腾讯控股", "浦发银行", "平安银行", "苹果"].slice(0, window.expandStocks ? 6 : 2).map(label => ({ id: label, label, value: "--", detail: "--", meta: "等待有效行情" })) };
      if (args.source !== "computer.status") throw Error(args.source);
      if (window.failSample) throw Error("采样中断");
      return { status: "ok", date: "2026-11-21", message: "示例采样 · 打开详情查看本机状态", rows: metrics.map(([label, value], i) => ({
        id: String(i), label, value, detail: i === 1 ? "6.7/16.0 GiB" : "",
        meta: ["处理器总占用", "已用 / 总量", "全部网络接口合计", "含虚拟接口，非测速", "电脑启动至今"][i] || "",
      })) };
    }, transformCallback: () => 1 };
    const React = (await import("/node_modules/.vite/deps/react.js")).default;
    const dom = await import("/node_modules/.vite/deps/react-dom_client.js");
    const { ComponentAppPreview } = await import("/src/component-center/AppSurface.jsx");
    const root = (dom.createRoot || dom.default.createRoot)(document.getElementById("root"));
    window.openComponent = component => root.render(React.createElement(ComponentAppPreview, {
      key: component.id, component, active: true, interactive: true,
    }));
    window.openComponent(component);
  }, await fixture("computer-status"));
  const tap = label => page.getByRole("button", { name: label, exact: true })
    .locator(':scope > rect[fill="transparent"]').click();
  const view = page.locator('[data-content-view="data"]');
  const visible = text => view.getByText(text, { exact: true }).waitFor();
  await visible("1 / 2");
  await visible("开机时长");
  await captureCover(page, "computer-status");
  await tap("下一页");
  await visible("2 / 2");
  await visible("可用内存");
  await visible("CPU 峰值");
  assert.equal(await view.getByText("开机时长", { exact: true }).count(), 0);
  await tap("回首页");
  await visible("1 / 2");
  await tap("上一页");
  await visible("2 / 2");
  await page.evaluate(() => { window.failSample = true; });
  await visible("数据已过期，请检查连接");
  await visible("CPU 峰值");
  await page.evaluate(component => window.openComponent(component), await fixture("stock-watchlist"));
  await visible("小米集团");
  await visible("阿里巴巴");
  await visible("1 / 1");
  assert.equal(await page.getByRole("button", { name: "下一页", exact: true }).getAttribute("aria-disabled"), "true");
  await captureCover(page, "stock-watchlist");
  await page.evaluate(() => { window.expandStocks = true; });
  await visible("1 / 2");
  await tap("下一页");
  await visible("苹果");
  await visible("2 / 2");
  await tap("回首页");
  await visible("小米集团");
  assert.deepEqual(errors, []);
  console.log("PASS: computer.status two pages, next, home, reverse wrap, stale last sample, stock runtime watchlist");
} finally {
  await browser.close();
}
