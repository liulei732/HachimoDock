import test from 'node:test';
import assert from 'node:assert/strict';
import {hasCodexAccounts,canDisplayEmail,displayChange,moveAccount,accountState,loginPhase} from './codex-accounts-model.js';

test('matches the registered provider without borrowing a stock ID or component title',()=>{
  const pcApplication={content:[{sources:{usage:{kind:'local-telemetry',source:'codex.accounts'}}}]};
  assert.equal(hasCodexAccounts({pcApplication}),true);
  assert.equal(hasCodexAccounts({id:'codex-quota-board'}),false);
  assert.equal(hasCodexAccounts({pcApplication:{content:[{sources:{usage:{kind:'constant',source:'codex.accounts'}}}]}}),false);
  assert.equal(hasCodexAccounts({dataSource:'stocks.watchlist'}),false);
});
test('display edits preserve ordering and revision without mutating saved state',()=>{
  const settings={revision:4,order:[2,1,3],visible:[2,1]},before=structuredClone(settings);
  assert.deepEqual(displayChange(settings,2,false),{operation:'configure',revision:4,order:[2,1,3],visible:[1]});
  assert.deepEqual(displayChange(settings,3,true).visible,[2,1,3]);
  assert.deepEqual(displayChange(settings,1,true).visible,[2,1]);
  assert.deepEqual(moveAccount(settings,1,-1).order,[1,2,3]);
  assert.deepEqual(moveAccount(settings,1,1).order,[2,3,1]);
  assert.equal(moveAccount(settings,2,-1),null);assert.equal(moveAccount(settings,3,1),null);
  assert.equal(moveAccount(settings,99,-1),null);assert.equal(moveAccount(settings,1,0),null);
  assert.deepEqual(settings,before);
});
test('checks exact email glyph coverage and handles missing fonts conservatively',()=>{
  const runtime={surface:{fonts:[{chars:'ab@.'},{chars:'com12'}]}};
  assert.equal(canDisplayEmail(runtime,'a12@b.com'),true);
  assert.equal(canDisplayEmail(runtime,'other@b.com'),false);
  assert.equal(canDisplayEmail(runtime,null),false);
  assert.equal(canDisplayEmail({},'x@example.com'),true);
  assert.equal(canDisplayEmail({},'中文@example.com'),false);
});
test('does not equate a saved login or stale quota with current server authorization',()=>{
  assert.equal(accountState({loginSaved:false}),'尚未登录');
  assert.equal(accountState({loginSaved:true}),'登录信息已保存 · 额度待刷新');
  assert.equal(accountState({loginSaved:true,status:'需重新登录'}),'需要重新授权');
  assert.equal(accountState({loginSaved:true,status:'过期 00:00',observedAt:123}),'最近结果已过期');
  assert.equal(accountState({loginSaved:true,status:'更新 00:00',observedAt:123}),'可用');
  assert.equal(loginPhase({phase:'cancelling'}),'正在取消登录…');
  assert.equal(loginPhase({phase:'error',error:'登录了另一个账号'}),'登录了另一个账号');
});
