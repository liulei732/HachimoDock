// Render the five authoring examples from their bundled initial state.
// Run against a local Vite server with an output directory for review.
import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import React from 'react';
import {renderToStaticMarkup} from 'react-dom/server';
import {AudioLines, BatteryMedium, Ear, NotebookPen, Thermometer} from 'lucide-react';
import {chromium} from 'playwright';

const [origin, output] = process.argv.slice(2);
if (!output || !/^http:\/\/127\.0\.0\.1:\d+$/.test(origin)) throw Error('Usage: node render-petui-example-covers.mjs http://127.0.0.1:PORT OUTPUT');
const examples = fileURLToPath(new URL('../skills/petui/examples/', import.meta.url));
const art = [
  ['ble-accessory-battery', BatteryMedium, '#a8e5a5'],
  ['ble-room-temperature', Thermometer, '#ffc78b'],
  ['mic-level-meter', AudioLines, '#99dfed'],
  ['quiet-room-check', Ear, '#c2baff'],
  ['local-notebook', NotebookPen, '#f4d995'],
];
await fs.mkdir(output, {recursive:true});
const browser = await chromium.launch({executablePath:process.env.PET_TEST_BROWSER, headless:true});
try {
  const page = await browser.newPage({viewport:{width:700,height:520},deviceScaleFactor:1});
  await page.route('**/__example_cover', route => route.fulfill({contentType:'text/html', body:
    '<html lang="zh"><head><link rel="stylesheet" href="/src/styles.css"></head><body style="margin:0"><main id="cover"></main></body></html>'}));
  for (const [id, Icon, accent] of art) {
    await page.goto(origin + '/__example_cover');
    const read = async name => JSON.parse(await fs.readFile(path.join(examples,id,name),'utf8'));
    const manifest = await read('component.json');
    const runtime = await read('runtime/widget.json');
    const dashboard = (await read('negative-screen.json')).dashboard;
    const icon = renderToStaticMarkup(React.createElement(Icon,{size:150,strokeWidth:1.9,color:'#182726'}));
    await page.evaluate(async ({manifest,runtime,dashboard,icon,accent,buttons}) => {
      const {default:refresh} = await import('/@react-refresh');
      refresh.injectIntoGlobalHook(window); window.$RefreshReg$=()=>{}; window.$RefreshSig$=()=>t=>t;
      window.__vite_plugin_react_preamble_installed__=true;
      const React = (await import('/node_modules/.vite/deps/react.js')).default;
      const dom = await import('/node_modules/.vite/deps/react-dom_client.js');
      const createRoot = dom.createRoot || dom.default.createRoot;
      const {default:Preview} = await import('/src/component-center/DeviceScreenPreview.jsx');
      const cover = document.querySelector('#cover');
      cover.style.cssText = 'position:relative;width:640px;height:480px;overflow:hidden;background:'+accent+';color:#182726;font-family:system-ui,sans-serif;';
      const label=document.createElement('h1'); label.textContent=manifest.name;
      label.style.cssText='position:absolute;top:34px;left:36px;margin:0;font-size:50px;line-height:1.2;color:#182726';
      cover.append(label);
      const symbol=document.createElement('div'); symbol.innerHTML=icon;
      symbol.style.cssText='position:absolute;left:33px;top:175px'; cover.append(symbol);
      const frame=document.createElement('div');
      frame.style.cssText='position:absolute;left:218px;top:137px;width:388px;height:291px;background:#111;border-radius:14px;overflow:hidden;box-shadow:0 6px 0 #182726';
      cover.append(frame);
      const root=document.createElement('div'); root.style.cssText='width:640px;height:480px;transform:scale(0.60625);transform-origin:top left';
      frame.append(root);
      const style=document.createElement('style');style.textContent='#cover .component-device-screen{width:640px;height:480px;border-radius:0}';document.head.append(style);
      createRoot(root).render(React.createElement(Preview,{cover:true,active:false,component:{...manifest,dashboard,appRuntime:runtime,mediaSurface:runtime.surface,sceneSprites:[],defaultBindings:buttons}}));
    }, {manifest,runtime,dashboard,icon,accent,buttons:await read('buttons.json')});
    await page.locator('[data-widget="'+id+'"]').waitFor();
    await page.evaluate(()=>document.fonts.ready);
    await page.locator('#cover').screenshot({path:path.join(output,id+'.png')});
  }
} finally { await browser.close(); }
