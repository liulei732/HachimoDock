import test from "node:test";
import assert from "node:assert/strict";
import { formatModelIssue } from "./model-issue.js";
import { realtimeErrorMessage, isRecoveringAsr, speechModelMessage } from "./realtime-feedback.js";
import { transcriptMessage } from "../dashboard/useDeviceVoiceRouter.js";
import { normalizeGenerationErrorMessage } from "./generation-issue.js";
import { createReferencePreviewController } from "./avatar-pipeline/reference-preview.js";

test("transcription and realtime use the originating model tag, including single business/WS codes", () => {
  for (const [stage, tag] of [["asr","ASR"],["tts","TTS"],["chat","LLM"],["storage","CREDENTIALS"]]) {
    const error = `[${tag}] HTTP 403 private-fixture-body`;
    const expected = formatModelIssue(stage, "", error);
    assert.equal(realtimeErrorMessage({state:"thinking", reason:"config", error}), expected);
    assert.equal(transcriptMessage({error,agentId:"deepseek-harness"}, "error", "visible"),expected);
    assert.doesNotMatch(expected, /private-fixture|DeepSeek/);
  }
  for (const [error, marker] of [["[TTS] WS 1008 服务拒绝","1008"],["[ASR] code=45000081 等待音频超时","45000081"]]) {
    const result=speechModelMessage(error);
    assert.equal(result,formatModelIssue("", "", error));
    assert.equal(result.split(marker).length,2);
  }
});

test("ambiguous turn, local playback and composer delivery keep their own causes", () => {
  const generic = realtimeErrorMessage({state:"speaking",reason:"config",error:"HTTP 429 upstream-fixture-body"});
  assert.match(generic,/HTTP 429/);
  assert.doesNotMatch(generic,/豆包|ASR|TTS|upstream-fixture|视频生成/);
  const local = "没有可用的音频输出设备，请检查扬声器。";
  assert.equal(speechModelMessage(local),local);
  assert.equal(transcriptMessage({failureDomain:"delivery",error:"请保持目标对话在前台"}, "error","visible"), "请保持目标对话在前台");
  const status={state:"listening",active:true,reason:"asr_timeout",error:"[ASR] code=45000081。仍在聆听，请再说一次。"};
  assert.equal(isRecoveringAsr(status),true);
  assert.match(realtimeErrorMessage(status),/ASR.*仍在聆听/);
  assert.equal(isRecoveringAsr({...status,error:"[LLM] HTTP 401"}),false);
});

test("image preview, custom video and unknown failures never inherit the task's video provider", async () => {
  let preview;
  const controller=createReferencePreviewController(value=>{preview=value;},async()=>{
    throw new Error("Ark image edit (ep-fixture) HTTP 403: private-fixture-body");
  });
  await controller.run({});
  assert.match(preview.error,/火山方舟.*图片生成.*HTTP 403/);
  const custom=normalizeGenerationErrorMessage("custom submit HTTP 401: private-fixture-body","volcengine");
  assert.match(custom,/自定义.*视频生成.*HTTP 401/);
  assert.doesNotMatch(custom,/火山|private-fixture/);
  const unknown=normalizeGenerationErrorMessage("HTTP 429: private-fixture-body","kling");
  assert.match(unknown,/HTTP 429/);
  assert.doesNotMatch(unknown,/可灵|火山|视频生成|private-fixture/);
  assert.equal(normalizeGenerationErrorMessage(Error("上次提交结果未知，请核对后恢复任务。")), "上次提交结果未知，请核对后恢复任务。");
});
