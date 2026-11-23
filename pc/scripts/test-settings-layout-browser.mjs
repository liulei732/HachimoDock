// Synthetic settings/UI regression; no credentials, network probes, or device operations.
import { chromium } from 'playwright';
import assert from 'node:assert/strict';
import { LLM_PRESETS } from '../src/lib/api-configuration.js';
const base = process.env.PET_TEST_URL || 'http://127.0.0.1:4173';
assert.equal(new URL(base).hostname, '127.0.0.1');
const browser = await chromium.launch({ headless: true, ...(process.env.PET_TEST_BROWSER ? { executablePath: process.env.PET_TEST_BROWSER } : {}) });
try {
  const page = await browser.newPage({ viewport: { width: 1180, height: 900 } });
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  const external = [];
  await page.route("**/*", route => {
    if (new URL(route.request().url()).origin === new URL(base).origin) return route.continue();
    external.push(new URL(route.request().url()).origin);
    return route.abort();
  });
  await page.route('**/__test-settings', r => r.fulfill({ contentType: 'text/html', body: '<html lang="zh"><body style="padding:24px;background:#f7f8fa"><div id="root"></div></body></html>' }));
  await page.goto(`${base}/__test-settings`);
  const installFixture = async (initial = {}) => {
    const Refresh = (await import('/@react-refresh')).default;
    Refresh.injectIntoGlobalHook(window); window.$RefreshReg$ = () => {}; window.$RefreshSig$ = () => type => type;
    window.__vite_plugin_react_preamble_installed__ = true;
    window.modelTestSettings = { llmConfigured: true, llmApiKeyMasked: 'tes…mask',
      llmApiKeyMasks: { deepseek: 'tes…mask', doubao: 'db…saved' }, llmProvider: 'deepseek', llmModel: 'deepseek-flash',
      ttsProvider: 'openai', ttsBaseUrl: 'https://previous-speech.invalid/v1', ttsModel: 'test-tts', llmSearchConfigured: true, ...initial.voice };
    const pendingVoiceSettings = new Promise(resolve => {
      window.finishVoiceSettings = () => resolve(window.modelTestSettings);
    });
    const customSpeechProfile = { baseUrl: 'https://speech.invalid/v1', model: 'test-asr' };
    let savedSpeech = { configured: true, provider: 'openai', ...customSpeechProfile, savedCustom: customSpeechProfile,
      savedCustomRoots: ['https://legacy-speech.invalid/v1'], savedVolcengine: false, savedMimo: false, ...initial.speech };
    let custom = initial.custom || {
      asr: { baseUrl: savedSpeech.savedCustom.baseUrl, model: savedSpeech.savedCustom.model, configured: true, active: savedSpeech.provider === 'openai', voice: '' },
      tts: { baseUrl: window.modelTestSettings.ttsBaseUrl, model: window.modelTestSettings.customTtsModel || 'test-tts',
        voice: window.modelTestSettings.customTtsVoice || '', configured: true, active: window.modelTestSettings.ttsProvider === 'openai' },
    };
    window.reopenSpeech = () => structuredClone({ speech: savedSpeech, custom, voice: window.modelTestSettings });
    window.customCalls = [];
    window.speechTestSaves = [];
    window.catalogCalls = 0;
    window.probes = [];
    window.speechProbes = 0;
    window.probeEvents = [];
    window.gen = Object.fromEntries(["volcengine", "kling", "custom"].map(id => [id, {
      apiKey: "fixture-generation-" + id, model: id === "kling" ? "kling-2.6" : "fixture-model",
      baseUrl: "https://old.invalid/v1", fastGeneration: true
    }]));
    window.__TAURI_INTERNALS__ = { invoke: async (command, args) => {
      if (command === "set_exit_reasons") return;
      if (command === "load_generation_provider_configs") return structuredClone(window.gen);
      if (command === "save_generation_provider_config") {
        if (window.failGen) throw Error('unavailable storage sk-fixture-sensitive');
        if (window.delayGen) await new Promise(resolve => { window.releaseGen = resolve; });
        window.gen[args.providerId] = structuredClone(args.config);
        return;
      }
      if (command === "http_request_text" || command === "list_voice_chat_models") {
        window.catalogCalls++;
        if (window.failCatalog) throw Error('HTTP 403 sk-fixture-sensitive');
        if (command === "list_voice_chat_models") return ["fixture-catalog"];
        return { status: 200, body: '{"data":[]}' };
      }
      if (command === "test_voice_chat_model") {
        window.probes.push(structuredClone(args.input));
        await new Promise(resolve => { window.releaseProbe = resolve; });
        if (window.failProbe) throw Error("fixture HTTP 403");
        return;
      }
      if (command === 'load_voice_chat_settings') return pendingVoiceSettings;
      if (command === 'save_voice_chat_settings') {
        if (window.failLlmSave) throw Error('unavailable storage sk-fixture-sensitive');
        if (args.input.ttsProvider) throw Error('Speech save must submit one transaction');
        window.modelTestSaved = args.input;
        const scope = args.input.llmProvider === 'custom' ? `custom:${args.input.llmBaseUrl.replace(/\/+$/, '')}` : args.input.llmProvider;
        const masked = window.modelTestSettings.llmApiKeyMasks[scope] || '';
        window.modelTestSettings = { ...window.modelTestSettings, ...args.input,
          llmConfigured: Boolean(masked), llmApiKeyMasked: masked };
        return window.modelTestSettings;
      }
      if (command === 'clear_voice_chat_secret') {
        if (args.which === 'llm') {
          window.forgotScope = args.scope;
          delete window.modelTestSettings.llmApiKeyMasks[args.scope];
          return structuredClone(window.modelTestSettings);
        }
        if (args.which !== 'search') throw Error('Wrong secret');
        window.searchCleared = true;
        return;
      }
      if (command === 'load_device_asr_settings') {
        if (initial.delayAsr) await new Promise(resolve => { window.finishAsrSettings = resolve; });
        return savedSpeech;
      }
      if (command === 'load_custom_speech_settings') return structuredClone(custom);
      if (command === 'save_custom_speech_stage') {
        const { stage, model, voice = '', baseUrl, apiKey } = args.input;
        if (!['asr', 'tts'].includes(stage)) throw Error('Unknown custom speech stage');
        if (!apiKey && (!custom[stage].configured || custom[stage].baseUrl !== baseUrl)) throw Error('请填写新地址的 API Key');
        window.customCalls.push([command, stage]);
        custom = { ...custom, [stage]: { model, voice, baseUrl, configured: true, active: true } };
        if (stage === 'asr') savedSpeech = { ...savedSpeech, provider: 'openai', savedProvider: 'openai' };
        else window.modelTestSettings = { ...window.modelTestSettings, ttsProvider: 'openai', ttsConfigured: true };
        return structuredClone(custom);
      }
      if (command === 'test_custom_speech_stage') {
        window.customCalls.push([command, args.stage]);
        return { stage: args.stage, latencyMs: 7 };
      }
      if (command === 'save_speech_settings') {
        if (window.failSpeechCommit) throw Error('fixture commit error');
        window.speechTestSaves.push(args.input);
        if (args.input.provider === 'volcengine') {
          const { apiKey } = args.input;
          if (!savedSpeech.savedVolcengine && !apiKey) throw new Error('Volcano credential missing');
          savedSpeech = { ...savedSpeech, provider: 'volcengine', savedVolcengine: true, baseUrl: '', model: '' };
        } else if (args.input.provider === 'mimo') {
          if (!savedSpeech.savedMimo && !args.input.apiKey) throw Error('MiMo credential missing');
          savedSpeech = { ...savedSpeech, provider: 'mimo', savedMimo: true, mimoAsrModel: args.input.model };
          window.modelTestSettings = { ...window.modelTestSettings,
            mimoTtsModel: args.input.ttsModel, mimoTtsVoice: args.input.ttsVoice };
        } else {
          throw Error('Custom speech must be saved by stage');
        }
        custom = { asr: { ...custom.asr, active: false }, tts: { ...custom.tts, active: false } };
        window.modelTestSettings.ttsProvider = savedSpeech.provider;
        savedSpeech.savedProvider = savedSpeech.provider;
        return savedSpeech;
      }
      if (command === 'test_device_asr_settings' || command === 'test_device_tts_settings') {
        window.probeEvents.push([command, savedSpeech.savedProvider || savedSpeech.provider, window.speechTestSaves.length]);
      }
      if (command === 'test_device_asr_settings') { window.speechProbes++;
        if (window.delaySpeech) await new Promise(resolve => { window.releaseSpeech = resolve; }); return { ok: !window.failSpeech, latencyMs: 5 }; }
      if (command === 'test_device_tts_settings') {
        if (window.failTts) throw Error('HTTP 403 sk-fixture-sensitive');
        return 6;
      }
      if (command === 'load_llm_network_settings') return { mode: 'system', proxyUrl: '', hasCertificate: false };
      if (command === 'save_llm_network_settings') return { ...args.input, certificateConfigured: false };
      if (command === 'test_llm_network_settings') return [
        { provider: '火山方舟', reachable: true, message: 'HTTPS 可达' },
        { provider: 'DeepSeek', reachable: false, message: 'TLS sk-fixture-sensitive' },
      ];
      throw new Error(`Unexpected command: ${command}`);
    } };
    const { mountSettings } = await import('/scripts/settings-browser-fixture.jsx');
    window.showButtons = await mountSettings();
  };
  await page.evaluate(installFixture);
  const speech = page.locator('.card').filter({ has: page.getByRole('heading', { name: '语音识别与合成', exact: true }) });
  const speechKey = speech.locator('#api-settings-asr-key');
  const volcanoSpeech = speech.getByRole('radio', { name: '豆包语音', exact: true });
  const customSpeech = speech.getByRole('radio', { name: /自定义端点/ });
  const mimoSpeech = speech.getByRole('radio', { name: '小米 MiMo', exact: true });
  const rows = speech.getByRole('list', { name: '语音测试分项结果' }).getByRole('listitem');
  const bothPassed = async () => {
    await rows.filter({ hasText: /语音识别.*通过/ }).waitFor();
    await rows.filter({ hasText: /语音合成.*通过/ }).waitFor();
  };
  const saveSpeech = speech.getByRole('button', { name: '保存并测试' });
  const defaults = { model: 'mimo-v2.5-asr', ttsModel: 'mimo-v2.5-tts', ttsVoice: 'mimo_default' };
  const disclosure = speech.locator('summary').filter({ hasText: /^模型与音色$/ });
  const fields = Object.fromEntries(Object.keys(defaults).map(field => [field, speech.locator('#api-settings-mimo-' + field)]));
  const readOptions = async () => Object.fromEntries(await Promise.all(Object.entries(fields)
    .map(async ([field, input]) => [field, await input.inputValue()])));
  const checkCollapsed = async () => {
    assert.equal(await disclosure.evaluate(el => el.parentElement.open), false);
    assert.equal(await fields.model.isVisible(), false);
    assert.equal(await speechKey.isVisible(), true);
  };
  await customSpeech.and(page.locator('[aria-checked=true]')).waitFor();
  assert.equal(await speechKey.count(), 0);
  await speech.getByRole('button', { name: '仅保存', exact: true }).nth(1).waitFor();
  assert.equal(await speech.getByRole('radio').count(), 3);
  await mimoSpeech.click();
  await checkCollapsed();
  assert.equal(await speechKey.inputValue(), '');
  await speech.getByText(/无需单独开通语音/).waitFor();
  await speech.getByText('https://api.xiaomimimo.com/v1', { exact: true }).waitFor();
  await disclosure.click();
  assert.deepEqual(await readOptions(), defaults);
  assert.ok((await fields.ttsVoice.evaluate(input => [...input.list.options].map(option => option.value))).includes('冰糖'));
  await fields.model.fill('draft-asr-before-read');
  await fields.ttsVoice.fill('用户先输入的音色');
  assert.equal(await saveSpeech.isDisabled(), true);
  await page.evaluate(() => {
    window.modelTestSettings.mimoTtsModel = 'saved-tts-from-inactive-mimo';
    window.modelTestSettings.mimoTtsVoice = '旧音色';
    window.finishVoiceSettings();
  });
  await page.waitForFunction(() => document.querySelector('#api-settings-mimo-ttsModel').value === 'saved-tts-from-inactive-mimo');
  assert.equal(await fields.ttsVoice.inputValue(), '用户先输入的音色');
  assert.equal(await fields.model.inputValue(), 'draft-asr-before-read');
  await customSpeech.click();
  assert.equal(await speechKey.count(), 0);
  await mimoSpeech.click();
  await disclosure.click();
  assert.equal(await fields.ttsVoice.inputValue(), '用户先输入的音色');
  await speech.getByRole('button', { name: '恢复默认' }).click();
  assert.deepEqual(await readOptions(), defaults);
  await disclosure.click();
  await saveSpeech.click();
  await speech.getByText(/请填写语音 API Key（小米 MiMo）/).waitFor();
  await speechKey.fill('unsaved-mimo-key');
  await volcanoSpeech.click();
  await speech.getByRole('link', { name: '豆包语音开通与密钥管理' }).waitFor();
  assert.equal(await speechKey.inputValue(), '');
  const card = page.locator('.card').filter({ has: page.getByRole('heading', { name: '对话大模型', exact: true }) });
  await card.getByRole('combobox', { name: '对话模型' }).and(page.locator(':enabled')).waitFor();
  assert.equal(await volcanoSpeech.getAttribute('aria-checked'), 'true');
  assert.equal(await speech.getByText('未配置', { exact: true }).count(), 1);
  await saveSpeech.click();
  assert.equal(await page.evaluate(() => window.speechTestSaves.length), 0);
  await speechKey.fill('unsaved-volcano-test-key');
  await customSpeech.click();
  assert.equal(await speechKey.count(), 0);
  await volcanoSpeech.click();
  assert.equal(await speechKey.inputValue(), '');
  await speechKey.fill('saved-volcano-fixture');
  await saveSpeech.click();
  await bothPassed();
  await mimoSpeech.click();
  await volcanoSpeech.click();
  assert.match(await speechKey.getAttribute('placeholder'), /留空可直接复测/);
  assert.equal(await speechKey.inputValue(), '');
  await saveSpeech.click();
  await bothPassed();
  const volcanoSave = { apiKey: null, provider: 'volcengine', baseUrl: null, model: null, ttsModel: null, ttsVoice: null };
  assert.deepEqual(await page.evaluate(() => window.speechTestSaves), [
    { ...volcanoSave, apiKey: 'saved-volcano-fixture' }, volcanoSave,
  ]);
  await card.getByRole('checkbox', { name: '手动填写模型 / 接入点 ID' }).check();
  const model = card.getByRole('textbox', { name: '对话模型自定义 ID' });
  const key = card.locator('#api-settings-llm-key');
  const search = card.getByRole('checkbox', { name: '按需联网检索' });
  assert.equal(await search.isChecked(), true);
  await card.getByRole('button', { name: '删除独立搜索 Key' }).click();
  await page.waitForFunction(() => window.searchCleared);
  await card.getByText('已删除独立搜索 Key。').waitFor();
  await search.uncheck();
  await card.getByRole('button', { name: '保存', exact: true }).click();
  await page.waitForFunction(() => window.modelTestSaved?.llmWebSearch === false);
  assert.equal(await search.isChecked(), false);
  assert.equal(await key.getAttribute('placeholder'), '已保存；留空保持不变');
  await key.fill('unsaved-test-key');
  await card.getByRole('radio', { name: '豆包', exact: true }).click();
  await card.getByRole('link', { name: '方舟模型开通与 API Key' }).waitFor();
  assert.equal(await key.inputValue(), '');
  assert.equal(await key.getAttribute('placeholder'), '已保存；留空保持不变');
  assert.match(await card.locator('.api-settings__result').first().innerText(), /已读取该服务/);
  await card.getByRole('radio', { name: 'DeepSeek', exact: true }).click();
  assert.equal(await key.getAttribute('placeholder'), '已保存；留空保持不变');
  await card.getByRole('radio', { name: 'MiMo', exact: true }).click();
  assert.equal(await key.getAttribute('placeholder'), '粘贴 MiMo 按量 API Key');
  await card.getByRole('radio', { name: 'DeepSeek', exact: true }).click();
  for (const { id, label: provider, model: expected } of LLM_PRESETS) {
    await card.getByRole('radio', { name: provider, exact: true }).click();
    assert.equal(await model.inputValue(), expected);
    await key.fill('fixture-chat-' + id);
    await card.getByRole('button', { name: '测试当前模型' }).click();
    await page.waitForFunction(() => window.releaseProbe);
    assert.deepEqual(await page.evaluate(() => window.probes.at(-1)), {
      llmProvider: id, llmModel: expected, llmBaseUrl: null, llmApiKey: 'fixture-chat-' + id,
    });
    await page.evaluate(() => { window.releaseProbe(); window.releaseProbe = null; });
    await card.getByText(/所选模型已返回测试文本/).waitFor();
    await model.fill('  account-model-id  ');
    await card.getByRole('radio', { name: provider, exact: true }).click();
    assert.equal(await model.inputValue(), '  account-model-id  ');
    await card.getByRole('button', { name: '保存', exact: true }).click();
    await page.waitForFunction(() => window.modelTestSaved?.llmModel === 'account-model-id');
    assert.equal(await model.inputValue(), 'account-model-id');
    assert.equal(await page.evaluate(() => window.modelTestSaved.llmBaseUrl), null);
    await page.evaluate(() => { window.modelTestSaved = null; });
  }
  await card.getByRole('combobox', { name: /MiMo 接入方式/ }).selectOption('mimo-token-plan-cn');
  await key.fill('fixture-token-plan');
  await model.fill('fixture-token-model');
  await card.getByRole('button', { name: '测试当前模型' }).click();
  await page.waitForFunction(() => window.releaseProbe);
  assert.equal(await key.isDisabled(), false);
  assert.equal(await card.getByRole('button', { name: '测试当前模型' }).isDisabled(), true);
  assert.equal(await page.evaluate(() => window.probes.at(-1).llmProvider), 'mimo-token-plan-cn');
  await model.fill('new-during-test');
  await page.evaluate(() => { window.failProbe = true; window.releaseProbe(); window.releaseProbe = null; });
  await card.getByText('配置已更改，请测试当前选择。').waitFor();
  assert.equal(await model.inputValue(), 'new-during-test');
  await card.getByRole('button', { name: '测试当前模型' }).click();
  await page.waitForFunction(() => window.releaseProbe);
  await page.evaluate(() => window.releaseProbe());
  await card.getByText(/MiMo Token Plan.*对话大模型.*HTTP 403/).waitFor();
  assert.equal(await model.inputValue(), 'new-during-test');
  assert.equal(await key.inputValue(), 'fixture-token-plan');
  await page.evaluate(() => { window.releaseProbe = null; window.failProbe = false; });
  await card.getByRole('button', { name: '测试当前模型' }).click();
  await page.waitForFunction(() => window.releaseProbe);
  await page.evaluate(() => { window.releaseProbe(); window.releaseProbe = null; });
  await card.getByText(/所选模型已返回测试文本/).waitFor();
  await model.fill('edited-after-success');
  await card.getByText('配置已更改，保存后生效。').waitFor();
  assert.equal(await page.evaluate(() => window.modelTestSaved), null);
  await card.getByRole('radio', { name: '自定义', exact: true }).click();
  assert.equal(await model.inputValue(), '');
  await card.getByLabel('接口地址（OpenAI 兼容）', { exact: true }).fill('https://example.invalid/v1');
  await model.fill('custom-model');
  await key.fill('fixture-custom');
  await card.getByRole('button', { name: '测试当前模型' }).click();
  await page.waitForFunction(() => window.releaseProbe);
  assert.equal(await page.evaluate(() => window.probes.at(-1).llmBaseUrl), 'https://example.invalid/v1');
  await page.evaluate(() => { window.releaseProbe(); window.releaseProbe = null; window.failLlmSave = true; });
  await card.getByText(/所选模型已返回测试文本/).waitFor();
  await card.getByRole('button', { name: '保存', exact: true }).click();
  await card.getByText(/保存失败.*本机配置/).waitFor();
  assert.equal(await key.inputValue(), 'fixture-custom');
  await page.evaluate(() => { window.failLlmSave = false; });
  await card.getByRole('button', { name: '保存', exact: true }).click();
  await page.waitForFunction(() => window.modelTestSaved?.llmModel === 'custom-model');
  assert.equal(await page.evaluate(() => window.modelTestSaved.llmBaseUrl), 'https://example.invalid/v1');
  await key.fill('custom-test-key');
  await card.getByLabel('接口地址（OpenAI 兼容）', { exact: true }).fill('https://new.invalid/v1');
  assert.equal(await key.inputValue(), '');
  assert.equal(await key.getAttribute('placeholder'), '粘贴这家模型的 API Key');
  assert.equal(await model.inputValue(), 'custom-model');
  await card.getByRole('radio', { name: 'MiMo', exact: true }).click();
  assert.match(await card.locator('.card__subtitle').innerText(), /不影响 ChatGPT、Claude 等 Agent 的语音输入/);
  assert.equal(await card.locator('.card__body').getByText('此配置用于和宠物聊天', { exact: false }).count(), 0);
  assert.equal(await card.locator('.usage-help').evaluate(el => getComputedStyle(el).margin), '0px');
  const contentGap = await card.evaluate(el => {
    const subtitle = el.querySelector('.card__subtitle').getBoundingClientRect();
    const first = el.querySelector('.card__body > :first-child').getBoundingClientRect();
    return first.top - subtitle.bottom;
  });
  assert.equal(contentGap, 8);
  const draftGaps = await page.locator('.api-settings__draft').evaluate(el => {
    const subtitle = el.closest('.page-shell').querySelector('.page-shell__subtitle').getBoundingClientRect();
    const draft = el.getBoundingClientRect();
    return [draft.top - subtitle.bottom, el.nextElementSibling.getBoundingClientRect().top - draft.bottom];
  });
  assert.deepEqual(draftGaps, [12, 12]);
  const subtitle = await card.locator('.card__subtitle').evaluate(el => ({height:el.getBoundingClientRect().height,lineHeight:parseFloat(getComputedStyle(el).lineHeight),maxWidth:getComputedStyle(el).maxWidth}));
  assert.equal(subtitle.maxWidth, 'none');
  assert.ok(subtitle.height <= subtitle.lineHeight + 1, 'desktop subtitle should use the full card width on one line');
  if (process.env.PET_TEST_SCREENSHOT) await card.screenshot({ path: process.env.PET_TEST_SCREENSHOT });
  const avatarModels = page.locator('#model-settings-avatar .video-model-row select');
  assert.equal(await avatarModels.count(), 3);
  for (const width of [760, 900, 1200, 1440]) {
    await page.setViewportSize({ width, height: 760 });
    const clipped = await avatarModels.evaluateAll(selects => selects
      .filter(select => select.scrollWidth > select.clientWidth + 1)
      .map(select => select.getAttribute('aria-label')));
    assert.deepEqual(clipped, [], `clipped avatar model at ${width}px`);
  }
  await page.setViewportSize({ width: 820, height: 700 });
  assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
  await mimoSpeech.click();
  const beforeProbes = await page.evaluate(() => window.speechProbes);
  const committed = await page.evaluate(() => window.speechTestSaves.length);
  await page.evaluate(() => { window.failSpeechCommit = true; });
  await speechKey.fill('pending-fixture-key');
  await saveSpeech.click();
  await speech.getByText(/语音配置保存失败.*本机配置/).waitFor();
  assert.equal(await speechKey.inputValue(), 'pending-fixture-key');
  assert.equal(await page.evaluate(() => window.speechTestSaves.length), committed);
  assert.equal(await page.evaluate(() => window.speechProbes), beforeProbes);
  await page.evaluate(() => { window.failSpeechCommit = false; });
  await speech.getByRole('button', { name: '仅保存' }).click();
  await speech.getByText('语音配置已保存。', { exact: true }).waitFor();
  assert.equal(await page.evaluate(() => window.speechProbes), beforeProbes);
  await page.evaluate(() => { window.failSpeech = true; });
  await saveSpeech.click();
  await rows.filter({ hasText: /语音识别.*有效文本/ }).waitFor();
  await rows.filter({ hasText: /语音合成.*通过/ }).waitFor();
  await page.evaluate(() => { window.delaySpeech = true; });
  await saveSpeech.click();
  await page.waitForFunction(() => window.releaseSpeech);
  await rows.filter({ hasText: /语音识别.*测试中/ }).waitFor();
  await rows.filter({ hasText: /语音合成.*通过/ }).waitFor();
  await volcanoSpeech.click();
  await page.evaluate(() => window.releaseSpeech());
  await speech.getByText('语音配置已更改，请保存并测试当前选择。').waitFor();
  assert.equal(await volcanoSpeech.getAttribute('aria-checked'), 'true');

  await page.evaluate(() => { window.delaySpeech = false; window.failSpeech = false; window.failTts = true; });
  await mimoSpeech.click();
  await checkCollapsed();
  assert.equal(await rows.count(), 0);
  await speechKey.fill('fixture-mimo-speech');
  await saveSpeech.click();
  await rows.filter({ hasText: /语音识别.*通过/ }).waitFor();
  await rows.filter({ hasText: /MiMo.*语音合成.*HTTP 403/ }).waitFor();
  const saveRow = await Promise.all([speech.getByRole('button', { name: '仅保存' }).boundingBox(), saveSpeech.boundingBox()]);
  assert.ok(Math.abs(saveRow[0].y - saveRow[1].y) <= 2, 'both speech actions fit alongside the key at 820px');
  if (process.env.PET_TEST_SPEECH_SCREENSHOT) await speech.screenshot({ path: process.env.PET_TEST_SPEECH_SCREENSHOT });
  assert.deepEqual(await page.evaluate(() => window.speechTestSaves.at(-1)), { provider: 'mimo', apiKey: 'fixture-mimo-speech', ...defaults });
  await page.evaluate(() => { window.failTts = false; });
  await customSpeech.click();
  assert.equal(await speechKey.count(), 0);
  await mimoSpeech.click();
  assert.equal(await speechKey.inputValue(), '');
  assert.match(await speechKey.getAttribute('placeholder'), /留空可直接复测/);
  await saveSpeech.click();
  await bothPassed();
  assert.deepEqual(await page.evaluate(() => window.speechTestSaves.at(-1)), { provider: 'mimo', apiKey: null, ...defaults });
  const last = await page.evaluate(() => window.speechTestSaves.length);
  assert.deepEqual(await page.evaluate(() => window.probeEvents.slice(-2)), [
    ['test_device_asr_settings', 'mimo', last], ['test_device_tts_settings', 'mimo', last],
  ]);
  assert.equal(await page.getByRole('button', { name: /显示密钥|隐藏密钥/ }).count(), 0);
  assert.equal(await page.getByRole('navigation', { name: '跳至模型配置项目' }).getByText('云服务网络').count(), 0);
  for (const id of ['volcengine','kling','custom']) {
    const section = page.locator('.api-settings__provider').filter({ has: page.locator('#api-settings-' + id + '-key') });
    const input = section.locator('#api-settings-' + id + '-key');
    assert.equal(await input.getAttribute('type'), 'password');
    assert.deepEqual(await input.evaluate(el => ['copy', 'cut', 'dragstart', 'paste'].map(type =>
      el.dispatchEvent(new Event(type, { bubbles: true, cancelable: true })))), [false, false, false, true]);
    assert.equal(await input.inputValue(), '');
    assert.match(await input.getAttribute('placeholder'), /已保存/);
    await section.getByRole('button', { name: '检查本地配置' }).click();
    await section.getByText(/本地配置检查通过/).waitFor();
    const checks = section.getByRole('list', { name: '本地配置检查分项' });
    assert.equal(await checks.getByRole('listitem').count(), { volcengine: 4, kling: 2, custom: 3 }[id]);
    await section.getByRole('button', { name: '保存', exact: true }).click();
    await section.getByText(/已保存到当前客户端/).waitFor();
    assert.equal(await page.evaluate(id => window.gen[id].apiKey, id), 'fixture-generation-' + id);
  }
  const custom = page.locator('.api-settings__provider').filter({ has: page.locator('#api-settings-custom-key') });
  await custom.locator('#api-settings-custom-url').fill('https://new.invalid/v1');
  await custom.getByRole('button', { name: '保存', exact: true }).click();
  await custom.getByText('请填写 API Key。').waitFor();
  await custom.locator('#api-settings-custom-key').fill('fixture-replacement');
  await page.evaluate(() => { window.failGen = true; });
  await custom.getByRole('button', { name: '保存', exact: true }).click();
  await custom.getByText(/保存失败.*本机配置/).waitFor();
  assert.equal(await custom.locator('#api-settings-custom-key').inputValue(), 'fixture-replacement');
  await page.evaluate(() => { window.failGen = false; window.delayGen = true; });
  await custom.getByRole('button', { name: '保存', exact: true }).click();
  await page.waitForFunction(() => window.releaseGen);
  await custom.locator('#api-settings-custom-model').fill('later-edit');
  await page.evaluate(() => window.releaseGen());
  await custom.getByText(/后续修改仍待保存/).waitFor();
  assert.equal(await custom.locator('#api-settings-custom-model').inputValue(), 'later-edit');
  assert.equal(await custom.locator('#api-settings-custom-key').inputValue(), '');
  assert.equal(await page.evaluate(() => window.gen.custom.model), 'fixture-model');
  assert.equal(await page.evaluate(() => window.gen.custom.apiKey), 'fixture-replacement');

  const ark = page.locator('#api-settings-volcengine-key');
  await ark.pressSequentially('fixture-provisional');
  await page.evaluate(() => window.dispatchEvent(new Event('focus')));
  await page.waitForTimeout(700);
  assert.equal(await page.evaluate(() => window.catalogCalls), 0);
  await page.getByRole('button', { name: '刷新可用模型', exact: true }).click();
  await page.waitForFunction(() => window.catalogCalls === 1);
  await ark.fill('fixture-changed');
  await ark.fill('fixture-provisional');
  await page.waitForTimeout(700);
  assert.equal(await page.evaluate(() => window.catalogCalls), 1);
  await page.evaluate(() => { window.failCatalog = true; });
  await page.getByRole('button', { name: '刷新可用模型', exact: true }).click();
  await page.getByRole('alert').filter({ hasText: /读取模型列表.*403/ }).waitFor();
  const network = page.locator('.llm-network-settings');
  await network.locator('summary').click();
  await network.locator('#llm-network-mode').selectOption('direct');
  assert.equal(await network.getByRole('button', { name: '测试已保存配置' }).isDisabled(), true);
  await network.getByRole('button', { name: '保存网络设置' }).click();
  await network.getByRole('button', { name: '测试已保存配置' }).click();
  await network.getByRole('listitem').filter({ hasText: /火山方舟.*HTTPS 可达/ }).waitFor();
  await network.getByRole('listitem').filter({ hasText: /DeepSeek.*无法验证服务证书/ }).waitFor();
  assert.ok(!(await page.locator('body').innerText()).includes('sk-fixture-sensitive'));
  await page.evaluate(() => window.showButtons());
  const summary = page.getByText('常用操作 · 按当前配置显示', { exact: true });
  await summary.waitFor();
  assert.equal(await summary.evaluate(el => el.parentElement.open), true);
  await summary.click();
  assert.equal(await summary.evaluate(el => el.parentElement.open), false);
  await summary.click();
  assert.equal(await summary.evaluate(el => el.parentElement.open), true);
  // The native reader decides whether an old custom profile is equivalent to the preset.
  // Exercise its public receipts here, including a different root and a custom voice.
  const official = 'https://api.xiaomimimo.com/v1';
  for (const [savedProvider, root, savedMimo, voice] of [
    ['mimo', official, true, 'mimo_default'],
    ['openai', 'https://token-plan-cn.xiaomimimo.com/v1', false, 'mimo_default'],
    ['openai', 'https://api.xiaomimimo.com.example.invalid/v1', false, 'mimo_default'],
    ['openai', official, false, '茉莉'], // A separate/ambiguous old TTS key has no reusable preset credential.
    ['openai', official, true, '茉莉'],  // An independently available preset key can coexist with custom speech.
  ]) {
    await page.reload();
    await page.evaluate(installFixture, {
      speech: { provider: savedProvider, savedProvider, savedMimo, savedCustom: { baseUrl: root, model: 'mimo-v2.5-asr' } },
      voice: { ttsProvider: savedProvider, ttsModel: 'mimo-v2.5-tts', ttsVoice: 'mimo_default',
        customTtsModel: 'mimo-v2.5-tts', customTtsVoice: voice },
    });
    await page.evaluate(() => window.finishVoiceSettings());
    await (savedProvider === 'mimo' ? mimoSpeech : customSpeech).and(page.locator('[aria-checked=true]:enabled')).waitFor();
    await customSpeech.click();
    assert.equal(await speechKey.count(), 0);
    await mimoSpeech.click();
    await checkCollapsed();
    assert.equal(await page.evaluate(() => window.speechTestSaves.length), 0);
    await saveSpeech.click();
    if (savedMimo) {
      await bothPassed();
      assert.deepEqual(await page.evaluate(() => window.speechTestSaves), [{ provider: 'mimo', apiKey: null, ...defaults }]);
    } else {
      await speech.getByText(/请填写语音 API Key（小米 MiMo）/).waitFor();
      assert.deepEqual(await page.evaluate(() => window.probeEvents), []);
      assert.equal(await page.evaluate(() => window.speechTestSaves.length), 0);
    }
  }
  async function reopen(initial) {
    await page.reload();
    await page.evaluate(installFixture, initial);
    await page.evaluate(() => window.finishVoiceSettings());
    await speech.getByRole('radio', { checked: true }).and(page.locator(':enabled')).waitFor();
  }
  const customAsr = speech.getByRole('group', { name: '语音识别（ASR）' });
  const customTts = speech.getByRole('group', { name: '语音合成（TTS）' });
  const draftNotice = page.locator('.api-settings__draft');
  const emptyStage = { baseUrl: '', model: '', voice: '', active: false, configured: false };
  await reopen({ speech: { provider: 'volcengine', savedVolcengine: false, configured: false },
    voice: { ttsProvider: 'volcengine', ttsConfigured: false }, custom: { asr: emptyStage, tts: emptyStage } });
  await mimoSpeech.click();
  await checkCollapsed();
  await speechKey.fill('first-mimo-fixture');
  await saveSpeech.click();
  await bothPassed();
  assert.deepEqual(await page.evaluate(() => window.speechTestSaves), [{ provider: 'mimo', apiKey: 'first-mimo-fixture', ...defaults }]);
  await customSpeech.click();
  assert.equal(await speechKey.count(), 0);
  await customAsr.getByLabel('接口地址').fill('https://asr.example.invalid/v1');
  await customAsr.getByLabel('模型 ID').fill('custom-asr');
  await customAsr.getByLabel('API Key', { exact: true }).fill('only-asr-fixture');
  await customAsr.getByRole('button', { name: '保存并测试' }).click();
  await customAsr.getByText(/已保存，测试通过/).waitFor();
  assert.equal(await customAsr.getByLabel('API Key', { exact: true }).inputValue(), '');
  await speech.getByText(/当前识别服务：自定义端点。当前合成服务：小米 MiMo/).waitFor();
  assert.deepEqual(await page.evaluate(() => window.reopenSpeech().custom.tts), emptyStage);
  assert.deepEqual(await page.evaluate(() => window.customCalls), [['save_custom_speech_stage', 'asr'], ['test_custom_speech_stage', 'asr']]);
  await reopen({ speech: { savedProvider: 'mimo', provider: 'mimo', savedMimo: true, mimoAsrModel: 'saved-inactive-asr' },
    voice: { ttsProvider: 'openai', mimoTtsModel: 'saved-inactive-tts', mimoTtsVoice: '保存的音色' } });
  await customSpeech.and(page.locator('[aria-checked=true]')).waitFor();
  await speech.getByText(/当前识别服务：小米 MiMo。当前合成服务：自定义端点/).waitFor();
  assert.equal(await speechKey.count(), 0);
  await customTts.getByLabel('模型 ID').fill('custom-draft-only');
  await mimoSpeech.click();
  await disclosure.click();
  assert.deepEqual(await readOptions(), { model: 'saved-inactive-asr', ttsModel: 'saved-inactive-tts', ttsVoice: '保存的音色' });
  const edited = { model: 'account-asr', ttsModel: 'account-tts', ttsVoice: '自选中文音色' };
  for (const [field, value] of Object.entries(edited)) await fields[field].fill(value);
  await volcanoSpeech.click();
  await mimoSpeech.click();
  await disclosure.click();
  assert.deepEqual(await readOptions(), edited);
  await page.evaluate(() => { window.failSpeechCommit = true; });
  await speech.getByRole('button', { name: '仅保存' }).click();
  await speech.getByText(/语音配置保存失败/).waitFor();
  assert.deepEqual(await readOptions(), edited);
  assert.deepEqual(await page.evaluate(() => window.probeEvents), []);
  await page.evaluate(() => { window.failSpeechCommit = false; });
  await speech.getByRole('button', { name: '仅保存' }).click();
  await speech.getByText('语音配置已保存。', { exact: true }).waitFor();
  assert.deepEqual(await page.evaluate(() => window.speechTestSaves.at(-1)), { provider: 'mimo', apiKey: null, ...edited });
  assert.deepEqual(await page.evaluate(() => window.probeEvents), []);
  await draftNotice.waitFor(); // The hidden custom TTS draft still needs saving.
  await customSpeech.click();
  assert.equal(await customTts.getByLabel('模型 ID').inputValue(), 'custom-draft-only');
  await customTts.getByLabel('接口地址').fill('https://another-speech.invalid/v1');
  await customTts.getByLabel('API Key', { exact: true }).fill('fixture-custom-tts');
  await customTts.getByRole('button', { name: '保存并测试' }).click();
  await customTts.getByText(/已保存，测试通过/).waitFor();
  assert.deepEqual(await page.evaluate(() => window.customCalls), [['save_custom_speech_stage', 'tts'], ['test_custom_speech_stage', 'tts']]);
  await speech.getByText(/当前识别服务：小米 MiMo。当前合成服务：自定义端点/).waitFor();
  await draftNotice.waitFor({ state: 'detached' });
  assert.equal(await customAsr.getByLabel('API Key', { exact: true }).inputValue(), '');
  await reopen(await page.evaluate(() => window.reopenSpeech()));
  await customSpeech.and(page.locator('[aria-checked=true]')).waitFor();
  await mimoSpeech.click();
  await checkCollapsed();
  await disclosure.click();
  assert.deepEqual(await readOptions(), edited);
  assert.equal(await fields.ttsVoice.evaluate(input => input.list), null);
  await fields.ttsModel.fill(defaults.ttsModel);
  await fields.ttsVoice.fill('冰糖');
  await fields.ttsModel.fill('mimo-v2.5-tts-voicedesign');
  const description = speech.getByLabel('音色设计描述', { exact: true });
  assert.equal(await description.evaluate(input => input.list), null);
  assert.equal(await description.inputValue(), ''); // The previous preset is not an instruction.
  await saveSpeech.click();
  await speech.getByText('请为音色设计模型填写音色描述。').waitFor();
  assert.equal(await page.evaluate(() => window.speechTestSaves.length), 0);
  await speech.getByRole('button', { name: '仅保存' }).click();
  await speech.getByText('语音配置已保存。', { exact: true }).waitFor();
  assert.equal(await description.inputValue(), defaults.ttsVoice);
  const savedWithoutDescription = await page.evaluate(() => window.speechTestSaves.length);
  await saveSpeech.click();
  await speech.getByText('请为音色设计模型填写音色描述。').waitFor();
  assert.equal(await page.evaluate(() => window.speechTestSaves.length), savedWithoutDescription);
  assert.deepEqual(await page.evaluate(() => window.probeEvents), []);
  const voiceDescription = '语速稍慢、清朗的青年声音';
  await description.fill(voiceDescription);
  await fields.ttsModel.fill(defaults.ttsModel);
  await fields.ttsVoice.fill('茉莉');
  await fields.ttsModel.fill('mimo-v2.5-tts-voicedesign');
  assert.equal(await description.inputValue(), voiceDescription);
  await fields.ttsModel.fill(defaults.ttsModel);
  assert.equal(await fields.ttsVoice.inputValue(), '茉莉');
  await fields.ttsModel.fill('mimo-v2.5-tts-voicedesign');
  await saveSpeech.click();
  await bothPassed();
  assert.deepEqual(await page.evaluate(() => window.speechTestSaves.at(-1)), {
    provider: 'mimo', apiKey: null, model: edited.model, ttsModel: 'mimo-v2.5-tts-voicedesign', ttsVoice: voiceDescription,
  });
  await reopen(await page.evaluate(() => window.reopenSpeech()));
  await mimoSpeech.and(page.locator('[aria-checked=true]')).waitFor();
  await checkCollapsed();
  await disclosure.click();
  assert.equal(await description.inputValue(), voiceDescription);
  await speech.getByRole('button', { name: '恢复默认' }).click();
  assert.deepEqual(await readOptions(), defaults);
  assert.equal(await description.count(), 0);
  assert.equal(await speech.getByLabel('合成音色', { exact: true }).and(fields.ttsVoice).isVisible(), true);
  assert.equal(await page.evaluate(() => window.speechTestSaves.length), 0);
  await draftNotice.waitFor();
  await saveSpeech.click();
  await bothPassed();
  await draftNotice.waitFor({ state: 'detached' });
  await reopen(await page.evaluate(() => window.reopenSpeech()));
  await disclosure.click();
  assert.deepEqual(await readOptions(), defaults);
  assert.equal(await speechKey.inputValue(), '');
  await page.reload();
  await page.evaluate(installFixture, { delayAsr: true,
    speech: { savedProvider: 'mimo', provider: 'mimo', savedMimo: true, mimoAsrModel: 'slow-old-asr' },
    voice: { ttsProvider: 'mimo', mimoTtsModel: 'fast-tts', mimoTtsVoice: '读回的音色' },
  });
  await page.evaluate(() => window.finishVoiceSettings());
  await disclosure.click();
  await fields.model.fill('new-asr-before-asr-read');
  assert.equal(await saveSpeech.isDisabled(), true);
  await page.evaluate(() => window.finishAsrSettings());
  await saveSpeech.and(page.locator(':enabled')).waitFor();
  assert.deepEqual(await readOptions(), { model: 'new-asr-before-asr-read', ttsModel: 'fast-tts', ttsVoice: '读回的音色' });
  console.log('PASS edited MiMo profiles, late ASR/TTS reads in both orders, default/reset flow, alternate Chinese voice and voice-design description, cold reopen, mixed providers and isolated ASR/TTS saves and probes.');
  await card.getByRole('radio', { name: '豆包', exact: true }).click();
  await card.getByRole('button', { name: '忘记此服务的 Key' }).click();
  await card.getByRole('button', { name: '取消', exact: true }).click();
  assert.equal(await page.evaluate(() => window.forgotScope), undefined);
  await card.getByRole('button', { name: '忘记此服务的 Key' }).click();
  await card.getByRole('button', { name: /确认忘记 火山方舟/ }).click();
  await card.getByText('已忘记此服务在本机保存的对话模型 Key。').waitFor();
  assert.equal(await page.evaluate(() => window.forgotScope), 'doubao');
  assert.equal(await card.getByRole('button', { name: '测试当前模型' }).isDisabled(), true);
  await card.getByRole('radio', { name: 'DeepSeek', exact: true }).click();
  assert.equal(await key.getAttribute('placeholder'), '已保存；留空保持不变');
  assert.deepEqual(external, []);
  assert.deepEqual(errors, []);
  console.log('PASS MiMo initial Key-only form, editable preset, migrated/native and custom receipts, address-scoped reuse, saved provider probes, independent 403, catalog and network results, zero external requests.');
  console.log('PASS generation fields and masked credentials for three providers, save failure and in-flight edits; atomic speech save, both partial probe directions and a pending ASR while TTS has finished.');
  console.log('PASS chat draft probe for all five connections, separate save failure and search-key deletion; catalog refresh only on explicit request, including a redacted rejection.');
  console.log('PASS settings: saved speech credentials follow the selected provider, and model editing, compact layout and common operations work.');
} finally { await browser.close(); }
