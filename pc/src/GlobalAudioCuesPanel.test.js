import test from 'node:test';
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
import {readFileSync} from 'node:fs';
import {runInNewContext} from 'node:vm';

const require = createRequire(import.meta.url);
const React = require('react');
const {renderToStaticMarkup} = require('react-dom/server');
const {buildSync} = require('esbuild');
const module = {exports: {}};
const bundle = buildSync({entryPoints: [fileURLToPath(new URL('./GlobalAudioCuesPanel.jsx', import.meta.url))],
  bundle: true, write: false, platform: 'node', format: 'cjs', external: ['react'], logLevel: 'silent'});
runInNewContext(bundle.outputFiles[0].text, {module, exports: module.exports, URL, TextEncoder,
  require: name => {assert.equal(name, 'react'); return React;}}, {filename: 'GlobalAudioCuesPanel.actual.cjs'});
const Panel = module.exports.default;
const selected = props => {
  const html = renderToStaticMarkup(React.createElement(Panel, props));
  return [...html.matchAll(/<option\b([^>]*)>/g)]
    .filter(([, attrs]) => /\bselected=""/.test(attrs))
    .map(([, attrs]) => attrs.match(/\bvalue="([^"]+)"/)[1]);
};

test('appearance sound controls initially select the actual action, including dotted idle and touch states', () => {
  for (const currentFamily of ['welcome', 'working', 'done', 'waiting_user', 'error',
    'idle', 'idle.default', 'idle.playing', 'touch.lick', 'permission_request']) {
    assert.deepEqual(selected({currentFamily}), [currentFamily]);
  }
});

test('standalone global controls retain their default without an appearance context', () => {
  for (const currentFamily of [undefined, '', 'unknown']) assert.deepEqual(selected({currentFamily}), ['done']);
});

test('appearance detail supplies the selected action to the global controls', () => {
  const source = readFileSync(new URL('./AppearanceDetail.jsx', import.meta.url), 'utf8');
  assert.match(source, /<GlobalAudioCuesPanel\b[^>]*currentFamily=\{activeFamily\}/);
});

test('wireless editing stays available and a stale wired id cannot enable global cue transfer', () => {
  for (const [props, enabled] of [
    [{wifiConnected: true}, false], [{wifiConnected: true, boardDeviceId: 'old'}, false],
    [{}, false], [{boardDeviceId: 'wired'}, true],
  ]) {
    const html = renderToStaticMarkup(React.createElement(Panel, props));
    const button = html.match(/<button[^>]*class="btn-primary"[^>]*>/)[0];
    assert.equal(!button.includes('disabled'), enabled);
    assert.doesNotMatch(html.match(/<input[^>]*type="file"[^>]*>/)[0], /disabled/);
    assert.equal(html.includes('下发全局提示音需要 USB'), props.wifiConnected === true);
  }
});
