// Run with node tests/gallery-session-recovery.mjs from pc. All requests are intercepted.
import assert from "node:assert/strict";
import { fileURLToPath } from "node:url";
import { build } from "esbuild";
import { chromium } from "playwright";

const project = fileURLToPath(new URL("../", import.meta.url));
const bundle = await build({
  absWorkingDir: project, entryPoints:["tests/fixtures/gallery-session.jsx"],
  bundle:true, write:false, define:{"process.env.NODE_ENV":'"production"'},
  plugins:[{name:"isolated-gallery-data",setup(builder) {
    builder.onResolve({filter:/\.\/lib\/appearance-store\.js$/}, args => {
      if (args.importer.endsWith("/AppearanceGallery.jsx")) return {path:"rows", namespace:"fixture"};
    });
    builder.onResolve({filter:/\.\/shell\/DeviceContext\.jsx$/}, () => ({path:"device", namespace:"fixture"}));
    builder.onLoad({filter:/^device$/,namespace:"fixture"}, () => ({
      contents:"export const useDeviceContext=()=>({currentDisplay:{}}), useUsageHelp=()=>({});",
    }));
    builder.onLoad({filter:/^rows$/,namespace:"fixture"}, () => ({
      resolveDir:project, contents:`
        export * from "./src/lib/appearance-store.js";
        const rows=Array.from({length:100},(_,i)=>({id:String(i),name:"Pet "+i,
          type:"custom",source_image:"fixture/"+i+".png",families:[],created_at:1}));
        export const getCachedAppearances=()=>rows, listAppearances=async()=>rows;
      `,
    }));
  }}],
});
const browser = await chromium.launch({executablePath:process.env.PET_TEST_BROWSER_EXECUTABLE || undefined});
try {
  const page = await browser.newPage({viewport:{width:1024,height:600}});
  const errors = [];
  page.on("pageerror", e => errors.push(e.message));
  await page.route("**/*", route => route.request().url() === "http://localhost/fixture"
    ? route.fulfill({contentType:"text/html",body:`<style>
      .appearance-grid{display:grid;grid-template-columns:repeat(4,1fr)}
      .appearance-card{height:275px}.appearance-card img{width:40px}
      </style><div id="root"></div>`})
    : route.abort());
  await page.goto("http://localhost/fixture");
  await page.addScriptTag({content:bundle.outputFiles[0].text});
  await page.evaluate(() => window.showFeed("A"));
  await page.waitForFunction(() => document.querySelector("output")?.textContent === "A");
  await page.evaluate(() => window.showFeed("B"));
  await page.waitForFunction(() => document.querySelector("output")?.textContent === "B");
  const elapsed = await page.evaluate(() => window.firstB - window.started);
  assert.ok(elapsed < 500, "100 ms snapshot should beat the held event request: " + elapsed);
  await page.evaluate(() => { window.releaseEvents("A"); window.releaseEvents("B"); });
  await page.waitForFunction(() => document.querySelector("output")?.textContent.includes("B-late"));
  assert.doesNotMatch(await page.locator("output").innerText(), /A/);
  const doneMs = await page.evaluate(() => window.bStates.find(s=>s.state==="done")?.time - window.burstStart);
  assert.ok(doneMs >= 0 && doneMs < 500, "terminal render latency: " + doneMs);
  assert.deepEqual(await page.evaluate(() => window.bStates.map(s=>s.state)), ["working","done","working"]);

  await page.evaluate(() => window.showFeed("C"));
  await page.waitForFunction(() => window.calls.some(c =>
    c.args?.route?.includes("agentId=C") && c.args.route.includes("cursor=1")));
  assert.equal(await page.locator("output").innerText(), "live-C");
  await page.evaluate(() => window.releaseSnapshot());
  await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
  assert.equal(await page.locator("output").innerText(), "live-C");

  await page.evaluate(() => window.showGallery());
  await page.getByRole("button",{name:"重新读取任务"}).waitFor();
  await page.locator(".appearance-card img").first().waitFor();
  const paths = () => page.evaluate(() => window.calls.filter(c=>c.command==="plugin:fs|read_file").map(c=>c.args.path));
  const first = await paths();
  assert.ok(first.length > 0 && first.length <= 16, "initial reads: " + first.length);
  assert.ok(!first.includes("fixture/99.png"));
  const last = page.locator(".appearance-card").last();
  await last.locator('[role="button"]').first().evaluate(el => el.focus({preventScroll:true}));
  await last.locator("img").waitFor({state:"attached"});
  assert.ok((await paths()).includes("fixture/99.png"));
  await last.scrollIntoViewIfNeeded();
  await page.waitForFunction(() => !document.querySelector(".appearance-card img")?.closest(".appearance-card")?.isSameNode(document.querySelector(".appearance-card")));
  await page.getByRole("button",{name:"重新读取任务"}).click();
  await page.getByText(/已保存 1\/11 段/).first().waitFor();
  assert.equal(await page.getByRole("button",{name:"重新读取任务"}).count(),0);
  const commands = await page.evaluate(() => [...new Set(window.calls.map(c=>c.command))]);
  assert.deepEqual(commands.sort(), ["load_voice_chat_settings","local_bridge_request","plugin:fs|read_file"].sort());
  assert.deepEqual(errors,[]);
  console.log(`PASS Chromium: B snapshot ${elapsed.toFixed(0)} ms; burst completion ${doneMs.toFixed(0)} ms; ${first.length}/100 initial reads; offscreen focus, scroll eviction and IndexedDB retry verified.`);
} finally { await browser.close(); }
