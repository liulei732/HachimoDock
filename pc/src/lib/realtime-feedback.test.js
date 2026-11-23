import test from "node:test";
import assert from "node:assert/strict";
import { isRecoveringAsr } from "./realtime-feedback.js";

test("ASR retry prompt remains a listening status, not a failed conversation", () => {
  const listening = { active: true, state: "listening", reason: "asr_timeout" };
  assert.equal(isRecoveringAsr(listening), true);
  for (const changed of [{ active: false }, { state: "ended" }, { reason: "asr" }, { reason: "config" }]) {
    assert.equal(isRecoveringAsr({ ...listening, ...changed }), false);
  }
  assert.equal(isRecoveringAsr(null), false);
});

test("native model stage distinguishes the same upstream HTTP error", async () => {
  const {realtimeErrorMessage} = await import("./realtime-feedback.js");
  const labels = ["asr", "tts", "chat"].map(errorStage =>
    realtimeErrorMessage({errorStage, error:"HTTP 404", reason:"config"}));
  assert.equal(new Set(labels).size, 3);
  for (const label of labels) assert.match(label, /404/);
});
