import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {canRepairThumbnail,syncAudioPreflight,deviceDeletePolicy,componentTransferError,componentCommitOutcomeUnknown,verifyWifiInstall} from './component-transfer-policy.js';

test('thumbnail repair requires a local cover, live installed version and capable transport', () => {
 const component={id:'example',version:'1',libraryPath:'/fixture',coverDataUrl:'data:image/png;base64,abc'};
 const inventory={scope:'board',freshness:'live',items:[{id:'example',version:'1'}]};
 const usb={capabilities:{widgetCenterThumb:'p4t-64x48-v1',widgetTransfer:'p4-widget-files-v2'}};
 const allowed=(c=component,i=inventory,t='usb',u=usb,w={})=>canRepairThumbnail(c,i,'board',{transport:t},u,w);
 assert.ok(allowed());
 assert.ok(allowed(component,{...inventory,items:[{id:'example'}]}));
 assert.ok(allowed(component,inventory,'wifi',null,{centerThumb:true}));
 assert.ok(!allowed(component,inventory,'wifi',null,{}));
 assert.ok(!allowed(component,inventory,'ssh'));
 assert.ok(!allowed(component,inventory,'usb',{capabilities:{widgetCenterThumb:true}}));
 assert.ok(!allowed({...component,coverDataUrl:null}));
 assert.ok(!allowed({...component,version:'2'}));
 assert.ok(!allowed(component,{...inventory,scope:'other'}));
 assert.ok(!allowed(component,{...inventory,freshness:'cached'}));
 assert.ok(!allowed(component,{...inventory,items:[]}));
});

test('sync audio preflight issues only a readonly correlated status request and preserves voice block',async()=>{
 const calls=[];
 const result=await syncAudioPreflight(async(command,input)=>{calls.push([command,input]);return {syncAudio:{blocked:true,reason:'voice'}};},'board-a');
 assert.equal(result.reason,'voice');
 assert.deepEqual(calls,[['usb_get_diagnostics',{expectedBoardDeviceId:'board-a',componentSync:true}]]);
});
test('unknown audio status fails closed before any installation',async()=>{
 await assert.rejects(syncAudioPreflight(async()=>({}),'board-a'),/尚未开始同步/);
 await assert.rejects(syncAudioPreflight(async()=>{throw Error('disconnected');},'board-a'),/disconnected/);
});
test('device saves default to preserve with exact advertised capability',()=>{
 assert.deepEqual(deviceDeletePolicy({transport:'usb',capabilities:{widgetDeletePreserveSave:true}}),{preserveSave:true,clearDeviceSaveConfirmed:false});
 for(const transport of ['usb','ssh'])assert.throws(()=>deviceDeletePolicy({transport,capabilities:{widgetDeletePreserveSave:'true'}}),/未删除/);
 assert.deepEqual(deviceDeletePolicy({clearSave:true,transport:'usb'}),{preserveSave:false,clearDeviceSaveConfirmed:true});
});
test('busy commit messages distinguish exclusive audio, storage, and ambiguous older firmware',()=>{
 const audio = componentTransferError('device audio is busy; catalog not committed; staged package preserved');
 assert.match(audio,/正在播放或使用语音/);
 const storage = componentTransferError(new Error('component storage is busy; commit has not started; staged package preserved'));
 assert.match(storage,/处理其他存储操作.*尚未开始安装/);
 assert.doesNotMatch(storage,/音频|语音/);
 assert.match(componentTransferError('component storage interrupted; catalog not committed; staged package preserved'),/存储操作中断/);
 for (const error of [
   'audio or component storage is busy; staged package preserved',
   'audio started; catalog commit deferred and staged package preserved',
 ]) {
   assert.match(componentTransferError(error),/可能是音频占用或组件存储忙/);
   assert.equal(componentCommitOutcomeUnknown(error),false);
 }
 assert.match(componentTransferError('component storage is busy'),/检查设备清单/);
 assert.match(componentTransferError('component storage is busy or recovery blocked'),/查看是否有存储异常/);
});
test('unknown commits require a fresh device inventory instead of an immediate retry',()=>{
 for (const error of [
   'widget commit acknowledgement timed out',
   '未收到板端组件 OTA 确认: transferId=widget-install-1 phase=commit',
   '组件提交结果尚未确认，请刷新设备组件清单核对。',
   '组件提交回执格式未确认，结果未知；不会自动重发或取消传输。',
   'component catalog outcome unknown; inspect only, do not replay',
   'durable catalog media publication failed; old runtime retained, recovery required',
 ]) {
   assert.equal(componentCommitOutcomeUnknown(error),true);
   assert.match(componentTransferError(error),/先刷新设备组件清单/);
 }
 assert.equal(componentCommitOutcomeUnknown('component storage is busy; commit has not started'),false);
});
test('USB automatically repeats only pre-write deferrals for the same staged package',()=>{
 const usb = readFileSync(new URL('../../src-tauri/src/usb_serial.rs', import.meta.url), 'utf8');
 const install = usb.split('fn install_widget_file_transfer<F>(', 2)[1].split('/// Transfer appearance', 1)[0];
 const commit = install.slice(install.indexOf('let commit ='));
 assert.match(install,/self\.send_inner\(topic, payload, true, Some\(board\), Some\(connection_id\), None\)/);
 assert.match(commit,/"transferId":id,"widgetId":widget,"packageSha256":digest/);
 assert.match(commit,/then_some\(attempt\)[\s\S]*payload\["index"\][\s\S]*send_phase\("widget\/commit", "commit", &payload, None, index/);
 assert.match(commit,/Duration::from_millis\(3500\)/);
 assert.match(commit,/ok == Some\(false\) && error_code == "commit_storage_deferred"[\s\S]*?<= retry_window[\s\S]*?continue;/);
 assert.match(commit,/None => \{\s*return match status\(\) \{\s*Ok\(WidgetTransferStatus::Committed\) => completed\(\),\s*_ => Err/);
 assert.equal(commit.match(/\bcontinue;/g)?.length, 1);
});

test('Wi-Fi commit uncertainty reads only the same live boot and confirms only a changed version', async () => {
  for (const error of ['无线安装提交结果未确认，请查看设备组件列表。','安装结果未确认。请查看设备组件列表，再决定是否重试。']) {
    assert.equal(componentCommitOutcomeUnknown(error),true);
  }
  assert.equal(componentCommitOutcomeUnknown('无线组件传输已中断，请检查连接。'),false);
  const check={scope:'wifi:A:boot1',id:'count',version:'2',previousVersion:'1'};
  const item={id:'count',version:'2'};
  const snapshot={scope:check.scope,items:[item],freshness:'live'};
  let scope=check.scope, reads=0;
  const read=async()=>{reads++;return snapshot;};
  assert.equal(await verifyWifiInstall(check,()=>scope,read),item);
  assert.equal(reads,1);
  await assert.rejects(verifyWifiInstall({...check,previousVersion:'2'},()=>scope,read),/同版本/);
  await assert.rejects(verifyWifiInstall(check,()=>scope,async()=>({...snapshot,items:[]})),/尚未确认/);
  await assert.rejects(verifyWifiInstall(check,()=>scope,async()=>({...snapshot,freshness:'stale'})),/最新/);
  await assert.rejects(verifyWifiInstall(check,()=>scope,async()=>{scope='wifi:A:boot2';return snapshot;}),/最新/);
  await assert.rejects(verifyWifiInstall(check,()=>scope,()=>assert.fail('must not read a different device')),/设备或连接已变化/);
});
