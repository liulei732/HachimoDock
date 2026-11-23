import test from 'node:test';
import assert from 'node:assert/strict';
import {formatModelIssue as explain} from './model-issue.js';

test('the operation and provider determine credential and entitlement guidance', () => {
  assert.match(explain('tts','火山引擎','HTTP 403'), /语音合成.*HTTP 403.*豆包语音.*语音合成 2.0/);
  assert.match(explain('asr','豆包语音','HTTP 401'), /ASR.*HTTP 401.*豆包语音 Key/);
  assert.match(explain('chat','豆包','HTTP 401'), /对话大模型.*火山方舟 Key/);
  assert.doesNotMatch(explain('asr','MiMo','HTTP 403'), /豆包|手动开通/);
});
test('only controlled labels, reasons and actual HTTP status reach the UI', () => {
  const secret='private-token-sentinel';
  for (const e of ['upstream content '+secret, new Error('HTTP/1.1 404 '+secret), {status:429,message:secret}]) {
    assert.doesNotMatch(explain('asr', 'https://'+secret, e), /sentinel|https:\/\//);
  }
  assert.doesNotMatch(explain('asr', '', '任务 401，持续 404 ms'), /HTTP|凭据/);
  assert.match(explain('video','', {status:429}), /限制.*并发/);
  assert.match(explain('asr','MiMo','等待音频超时'), /检查录音/);
});

test('native failure tags distinguish service codes and HTTP failures from storage and WS', () => {
  assert.match(explain('tts','MiMo','[ASR] code=invalid_api_key private'), /ASR.*服务错误码 invalid_api_key.*凭据/);
  assert.match(explain('asr','豆包语音','[TTS] code=403'), /TTS.*服务错误码 403.*语音合成 2.0/);
  assert.match(explain('chat','','[CREDENTIALS] 无法解密配置'), /本机配置.*解锁/);
  assert.match(explain('tts','','[TTS] WS 1011 private'), /WebSocket 关闭码 1011.*关闭了连接/);
  assert.match(explain('tts','','[TTS] code=45000081'), /录音输入/);
  assert.doesNotMatch(explain('asr','','[ASR] code=unknown access=private'), /private|HTTP|unknown/);
  assert.match(explain('network','MiMo',{status:403}), /无凭据.*检测被拒绝/);
});
