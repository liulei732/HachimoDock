import test from 'node:test';
import assert from 'node:assert/strict';
import {builtinFeatureManifest} from './feature-manifest.js';
import {featureView,featureTarget} from './component-features.js';
import cases from '../../skills/petui/contracts/feature-compatibility-cases.json' with {type:'json'};

for (const sample of cases) test('builtin/desktop/public feature contract: '+sample.case,()=>{
  const profile=builtinFeatureManifest('test',sample.runtime,sample.app,false);
  assert.deepEqual(profile.features.map(r=>r.id),sample.ids);
  for (const target of sample.targets) {
    const view=featureView(profile,target.diagnostic);
    assert.deepEqual(view.rows.find(r=>r.id===target.feature).paths,target.paths);
  }
});

test('different-board and stale-boot network status do not follow USB selection',()=>{
  const usb={connected:true,boardDeviceId:'a',bootId:'one',capabilities:{}};
  const wifi={controlTrusted:true,wifiConnected:true,boardDeviceId:'b',bootId:'one'};
  assert.equal(featureTarget(usb,wifi).wifiConnected,undefined);
  assert.equal(featureTarget(usb,{...wifi,boardDeviceId:'a',bootId:'two'}).wifiConnected,undefined);
  assert.equal(featureTarget(usb,{...wifi,boardDeviceId:'a'}).wifiConnected,true);
  assert.equal(featureTarget(null,{...wifi,controlTrusted:false}),null);
});

test('filters expose multiple producers and remove unsupported paths for Pico',()=>{
  const stock=cases.find(c=>c.case==='stock'), manifest=builtinFeatureManifest('stock',stock.runtime);
  assert.deepEqual(featureView(manifest).modes,['network','pc']);
  const d=stock.targets.find(t=>t.diagnostic.capabilities.firmwareUpdate.hardwareBoardModel==='pico').diagnostic;
  assert.deepEqual(featureView(manifest,d).modes,['pc']);
  assert.deepEqual(featureView(null,d).modes,['unknown']);
});

test("optional narration leaves the device operation independent of PC speech", () => {
  const manifest = builtinFeatureManifest("reader", {engine:"p4",local:{engine:"reader"}}, {autoRead:true});
  const view = featureView(manifest);
  assert.deepEqual(view.modes, ["offline"]);
  assert.match(view.summary, /可选朗读/);
  assert.match(view.rows.find(row => row.id === "optional-narration").label, /TTS/);
});
