// Mounted gallery editor, with the native filesystem, configuration and audio IPC kept in memory.
import assert from "node:assert/strict";
import { chromium } from "playwright";

const origin = process.env.PET_TEST_URL || "http://127.0.0.1:4173";
assert.equal(new URL(origin).hostname, "127.0.0.1");
const browser = await chromium.launch({ headless: true, executablePath: process.env.PET_TEST_BROWSER });
try {
  const page = await browser.newPage({ viewport: { width: 1100, height: 820 } });
  page.setDefaultTimeout(8000);
  const errors = [];
  page.on("pageerror", error => errors.push(error.message));
  await page.route("**/*", route => {
    const url = new URL(route.request().url());
    if (url.origin !== origin) return route.abort();
    if (url.pathname === "/__persona") return route.fulfill({ contentType: "text/html", body: '<div id="root"></div>' });
    return route.continue();
  });
  await page.goto(origin + "/__persona");
  await page.evaluate(async () => {
    const refresh = (await import("/@react-refresh")).default;
    refresh.injectIntoGlobalHook(window);
    window.$RefreshReg$ = () => {};
    window.$RefreshSig$ = () => type => type;
    window.__vite_plugin_react_preamble_installed__ = true;
    window.statusFixture = { ttsConfigured: true, ttsProvider: "mimo", ttsModel: "mimo-v2.5-tts", ttsVoice: "Mia" };
    window.__TAURI_INTERNALS__ = { invoke: async (command, args) => {
      switch (command) {
        case "load_voice_chat_settings":
          if (window.blockNextRead) {
            window.blockNextRead = false;
            return new Promise(resolve => { window.resolveOldRead = resolve; });
          }
          if (window.readFailed) throw Error("fixture read failed");
          return { ...window.statusFixture };
        case "load_realtime_voice_volume": return 0.7;
        case "reserve_persona_voice_preview": {
          const id = crypto.randomUUID();
          if (window.holdReservation) await new Promise(resolve => {
            window.reservedId = id;
            window.releaseReservation = resolve;
          });
          return id;
        }
        case "cancel_persona_voice_preview":
          window.cancelledPreview = args.previewId;
          window.finishPreview?.();
          return;
        case "persona_voice_preview":
          window.preview = args.input;
          window.previewId = args.previewId;
          if (window.holdPreview) await new Promise(resolve => { window.finishPreview = resolve; });
          return;
        case "plugin:fs|exists": return true;
        case "plugin:fs|read_dir": return [];
        case "plugin:path|resolve_directory": return "/fixture";
        case "plugin:fs|read_text_file": return new TextEncoder().encode(JSON.stringify(
          args.path.endsWith("persona-voice.json") ? window.documentFixture : {}));
        case "plugin:fs|write_text_file": window.documentFixture = JSON.parse(new TextDecoder().decode(args)); return;
        case "realtime_chat_update_persona": window.updated = args.input; return;
        case "save_realtime_voice_volume":
        case "set_exit_reasons": return;
        default: throw Error("Unhandled fixture IPC " + command);
      }
    } };
    (await import("/scripts/fixtures/persona-gallery.jsx")).mount();
  });
  const dialog = page.getByRole("dialog");
  const choices = dialog.getByRole("combobox", { name: "形象音色", exact: true });
  const manual = dialog.getByRole("textbox", { name: /形象音色 ID/ });
  const preview = dialog.getByRole("button", { name: "试听", exact: true });
  const search = dialog.getByRole("searchbox");
  await choices.waitFor();
  assert.equal(await choices.inputValue(), "");
  assert.equal(await choices.locator("option").count(), 11);
  assert.equal(await search.count(), 0);
  await page.evaluate(() => { window.holdPreview = true; });
  await preview.click();
  await dialog.getByRole("button", { name: "停止试听" }).click();
  await page.waitForFunction(() => window.cancelledPreview === window.previewId);
  await preview.click();
  await dialog.getByRole("button", { name: "关闭", exact: true }).click();
  await dialog.waitFor({ state: "hidden" });
  await page.waitForFunction(() => window.cancelledPreview === window.previewId);
  await page.evaluate(() => window.openEditor(3, true));
  await page.evaluate(() => { window.holdReservation = true; });
  await preview.click();
  await page.waitForFunction(() => !!window.releaseReservation);
  const previousDialog = await dialog.elementHandle();
  await page.evaluate(() => window.openEditor(4, true));
  await page.waitForFunction(el => !el.isConnected, previousDialog);
  await page.evaluate(() => window.releaseReservation());
  await page.waitForFunction(() => window.cancelledPreview === window.reservedId);
  assert.equal(await page.evaluate(() => window.previewId === window.reservedId), false);
  await page.evaluate(() => { window.holdPreview = false; window.holdReservation = false; });
  await choices.selectOption({ label: "使用 MiMo 默认音色" });
  await preview.click();
  assert.equal(await page.evaluate(() => window.preview.voice.ttsOverrides.mimo.voice), "mimo_default");
  await choices.selectOption("");
  await preview.click();
  assert.equal(await page.evaluate(() => window.preview.voice.ttsOverrides), undefined);
  await choices.selectOption("@manual");
  await manual.fill("供应商新音色");
  await preview.click();
  assert.equal(await page.evaluate(() => window.preview.voice.ttsOverrides.mimo.voice), "供应商新音色");
  await choices.selectOption("茉莉");
  await page.evaluate(() => window.changeRoute({ ttsProvider: "volcengine" }));
  await search.fill("佩奇");
  const clone = dialog.getByRole("textbox", { name: /复刻音色 ID/ });
  await clone.fill("S_fixture");
  await page.evaluate(() => window.changeRoute({ ttsProvider: "openai", ttsModel: "custom", ttsVoice: "global-custom",
    ttsBaseUrl: "https://fixture.invalid/service" }, "focus"));
  await manual.fill("custom-pet");
  assert.equal(await clone.count(), 0);
  await preview.click();
  const slots = { mimo: { model: "mimo-v2.5-tts", voice: "茉莉" }, openai: {
    model: "custom", baseUrl: "https://fixture.invalid/service/v1/audio/speech", voice: "custom-pet" } };
  assert.deepEqual(await page.evaluate(() => window.preview.voice.ttsOverrides), slots);
  await dialog.getByRole("button", { name: "保存", exact: true }).click();
  await dialog.waitFor({ state: "hidden" });
  const stored = await page.evaluate(() => window.documentFixture.voice);
  assert.deepEqual(stored.tts_overrides, slots);
  assert.equal(stored.clone_speaker_id, "S_fixture");
  assert.ok(!Object.hasOwn(stored, "volume"));
  assert.deepEqual(await page.evaluate(() => window.updated.voice.ttsOverrides), slots);

  await page.evaluate(() => window.openEditor(2, false));
  await manual.waitFor();
  assert.equal(await manual.inputValue(), "custom-pet");
  assert.equal(await preview.isDisabled(), true);
  await page.evaluate(() => window.changeRoute({ ttsBaseUrl: "https://fixture.invalid/service/v1/chat/completions" }));
  await page.waitForFunction(() => document.querySelector('input[placeholder="输入此服务支持的音色 ID"]').value === "");
  await page.evaluate(() => {
    window.blockNextRead = true;
    window.changeRoute({});
    window.changeRoute({ ttsProvider: "mimo", ttsModel: "mimo-v2.5-tts", ttsVoice: "Mia" }, "focus");
  });
  await choices.waitFor();
  assert.equal(await choices.inputValue(), "茉莉");
  await page.evaluate(() => window.resolveOldRead({ ttsConfigured: true, ttsProvider: "volcengine" }));
  assert.equal(await choices.inputValue(), "茉莉");
  await page.evaluate(() => window.changeRoute({ ttsModel: "mimo-v2.5-tts-voicedesign", ttsVoice: "mimo_default" }));
  const description = dialog.getByRole("textbox", { name: /形象声音描述/ });
  await description.waitFor();
  await page.evaluate(() => window.openEditor(2, true));
  assert.equal(await preview.isDisabled(), true);
  await description.fill("温和的青年男声，慢慢说。");
  assert.equal(await choices.count(), 0);
  await preview.click();
  assert.equal(await page.evaluate(() => window.preview.voice.ttsOverrides.mimo.voice), "温和的青年男声，慢慢说。");
  await description.fill("中".repeat(342));
  await dialog.getByRole("button", { name: "保存", exact: true }).click();
  await dialog.getByText(/超过 1024 字节/).waitFor();
  assert.deepEqual(await page.evaluate(() => window.documentFixture.voice.tts_overrides), slots);
  await page.evaluate(() => { window.readFailed = true; window.changeRoute({}); });
  await dialog.getByText("当前合成服务未知").waitFor();
  assert.equal(await preview.isDisabled(), true);
  assert.equal(await search.count(), 0);
  await page.evaluate(async () => {
    window.readFailed = false;
    window.changeRoute({ ttsProvider: "mimo", ttsModel: "mimo-v2.5-tts" });
    await window.openGallery();
  });
  const card = page.locator(".appearance-card__persona");
  try {
    await card.filter({ hasText: /^\s*小西 · MiMo · 茉莉\s*$/ }).waitFor();
  } catch (error) {
    throw Error(JSON.stringify({ cards: await card.allTextContents(), errors, body: (await page.locator("body").innerText()).slice(0, 300) }), { cause: error });
  }
  await page.evaluate(() => window.changeRoute({ ttsProvider: "volcengine" }, "focus"));
  await card.filter({ hasText: /^\s*小西 · 复刻音色\s*$/ }).waitFor();
  await page.evaluate(() => { window.readFailed = true; window.changeRoute({}); });
  await card.filter({ hasText: /^\s*小西\s*$/ }).waitFor();
  assert.deepEqual(errors, []);
  console.log("PASS mounted persona editor: scoped presets, inheritance, manual IDs, clone, native preview/save/live update, reopen, focus/event race, voice design, byte limit, preview gate and gallery card refresh");
} finally {
  await browser.close();
}
