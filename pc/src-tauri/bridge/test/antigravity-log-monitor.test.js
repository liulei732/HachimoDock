"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const test = require("node:test");
const { Worker } = require("node:worker_threads");
const AntigravityLogMonitor = require("../agents/antigravity-log-monitor");
const { parseAntigravityStep } = require("../agents/antigravity-transcript");
const {
  antigravityMonitorEnabled,
  LocalStatePublisher,
  publishAntigravityState,
} = require("../packages/clawd-backend-service/src/headless-mqtt");

const prompt = (content) => ({ type: "USER_INPUT", content });
const answer = (content) => ({ type: "PLANNER_RESPONSE", status: "DONE", content });
const jsonl = (...rows) => rows.map((row) => JSON.stringify(row) + "\n").join("");

function fixture(t, idleClearMs) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "antigravity-"));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const brainDir = path.join(root, ".gemini", "antigravity", "brain");
  const transcript = (session) => {
    const directory = path.join(brainDir, session, ".system_generated", "logs");
    fs.mkdirSync(directory, { recursive: true });
    return path.join(directory, "transcript.jsonl");
  };
  const events = [];
  // Choose an exact whole second so transcript mtimes and the idle timer
  // can use the same simulated clock on Windows and Unix.
  let now = Date.UTC(2024, 0, 1);
  function writeCurrent(session, contents) {
    const file = transcript(session);
    fs.writeFileSync(file, contents);
    fs.utimesSync(file, now / 1000, now / 1000);
  }
  const monitor = new AntigravityLogMonitor(
    { brainDir, now: () => now, idleClearMs },
    (session, state, event, { display }) => events.push({ session, state, event, ...display }),
  );
  return { root, brainDir, transcript, writeCurrent, monitor, events, advance(ms) { now += ms; monitor.poll(); } };
}

test("parser covers working, tools, partial reply, completed and error with bounded public fields", () => {
  assert.equal(parseAntigravityStep("not JSON"), null);
  assert.equal(parseAntigravityStep("null"), null);
  const rows = [
    prompt("private wrapper<USER_REQUEST>修复连接</USER_REQUEST>"),
    { type: "PLANNER_RESPONSE", thinking: "PRIVATE_THINKING" },
    { type: "PLANNER_RESPONSE", tool_calls: [{ name: "run_command", args: "PRIVATE_ARGUMENTS" }] },
    { type: "GENERIC", content: "PRIVATE_TOOL_OUTPUT" },
    { ...answer("已完成 😀"), status: "RUNNING" },
    { type: "PLANNER_RESPONSE", status: "DONE" },
    { type: "PLANNER_RESPONSE", status: "ERROR", content: "PRIVATE_ERROR_DETAIL" },
  ];
  let previous;
  const parsed = rows.map((row) => {
    previous = parseAntigravityStep(JSON.stringify(row), previous);
    return previous;
  });
  assert.deepEqual(parsed.map(({ state, event, title, content }) => [state, event.split(":")[1], title, content]), [
    ["working", "user_input", "修复连接", "正在思考"],
    ["working", "thinking", "修复连接", "正在思考"],
    ["working", "tool_call", "修复连接", "运行命令"],
    ["working", "tool_result", "修复连接", "正在处理工具结果"],
    ["speaking", "response", "修复连接", "已完成 😀"],
    ["done", "completed", "修复连接", "已完成 😀"],
    ["error", "error", "修复连接", "执行失败"],
  ]);
  assert.equal(Buffer.byteLength(parseAntigravityStep(JSON.stringify(answer("读".repeat(500)))).content), 381);
});

test("baseline skips history and a partial UTF-8 append emits exactly once after completion", (t) => {
  const { transcript, monitor, events } = fixture(t);
  const file = transcript("session-a");
  fs.writeFileSync(file, jsonl(prompt("history"), answer("prior answer")));
  monitor.poll();
  assert.deepEqual(events.map((event) => event.event), ["antigravity:ready"]);
  const row = Buffer.from(jsonl(prompt("中文续轮")));
  const cut = row.indexOf(Buffer.from("文")) + 1;
  fs.appendFileSync(file, row.subarray(0, cut));
  monitor.poll();
  assert.equal(events.length, 1);
  fs.appendFileSync(file, row.subarray(cut));
  monitor.poll();
  monitor.poll();
  assert.equal(events.length, 2);
  assert.equal(events.at(-1).title, "中文续轮");
  assert.equal(events.at(-1).state, "working");
});

test("trim, same-length rewrite, replacement and truncate recover without replay", async (t) => {
  const { transcript, monitor, events } = fixture(t);
  const file = transcript("session-a");
  const old = prompt("history");
  const shared = answer("shared answer");
  fs.writeFileSync(file, jsonl(old, shared));
  // Exercise the actual polling interval. Synchronous writes and manual polls
  // in one clock tick can share a timestamp on Windows.
  monitor.start();
  t.after(() => monitor.stop());
  fs.writeFileSync(file, jsonl(shared, prompt("new turn")));
  await t.waitFor(() => assert.equal(events.at(-1)?.title, "new turn"), { timeout: 5000 });
  assert.equal(events.at(-1).content, "正在思考");

  fs.writeFileSync(file, jsonl(shared, prompt("new edit")));
  await t.waitFor(() => assert.equal(events.at(-1)?.title, "new edit"), { timeout: 5000 });

  fs.writeFileSync(file + ".next", jsonl(prompt("new edit"), answer("rotated reply")));
  // unlink + rename also works on Windows when the file is replaced externally.
  fs.unlinkSync(file);
  fs.renameSync(file + ".next", file);
  monitor.poll();
  fs.truncateSync(file, 0);
  monitor.poll();
  fs.writeFileSync(file, jsonl(answer("rotated reply"), prompt("after truncate")));
  monitor.poll();

  assert.deepEqual(events.slice(1).map(({ state, title, content }) => [state, title, content]), [
    ["working", "new turn", "正在思考"],
    ["working", "new edit", "正在思考"],
    ["done", "new edit", "rotated reply"],
    ["working", "after truncate", "正在思考"],
  ]);
});

test("the most recent conversation renews its bubble and only it can clear it", (t) => {
  const { transcript, writeCurrent, monitor, events, advance } = fixture(t, 30_000);
  monitor.poll();
  writeCurrent("old", jsonl(prompt("old"), answer("old answer")));
  monitor.poll();
  advance(15_000);
  writeCurrent("new", jsonl(prompt("new")));
  monitor.poll();
  advance(15_000);
  assert.equal(events.at(-1).session, "new");
  assert.equal(events.at(-1).state, "working");
  fs.appendFileSync(transcript("new"), jsonl(answer("new answer")));
  monitor.poll();
  advance(10_000);
  assert.equal(events.at(-1).content, "new answer");
  advance(20_000);
  assert.equal(events.at(-1).event, "antigravity:idle_clear");
  assert.equal(events.at(-1).content, "");
});

test("a completed reply uses the standard sixty-second bubble lifetime by default", (t) => {
  const { writeCurrent, monitor, events, advance } = fixture(t);
  monitor.poll();
  writeCurrent("session", jsonl(answer("completed reply")));
  monitor.poll();
  advance(59_999);
  assert.equal(events.at(-1).content, "completed reply");
  advance(1);
  assert.equal(events.at(-1).event, "antigravity:idle_clear");
});

test("an idle-clear timeout of zero preserves the reply", (t) => {
  const { writeCurrent, monitor, events, advance } = fixture(t, 0);
  monitor.poll();
  writeCurrent("session", jsonl(answer("keep this")));
  monitor.poll();
  advance(1_000_000);
  assert.equal(events.at(-1).content, "keep this");
});

test("bridge requires native detection and an explicit Antigravity selection", () => {
  for (const CLAWD_SELECTED_AGENT_ID of ["", "workbuddy", "antigravity"]) {
    for (const CLAWD_ENABLE_ANTIGRAVITY_MONITOR of [undefined, "false", "true"]) {
      assert.equal(antigravityMonitorEnabled({
        CLAWD_SELECTED_AGENT_ID, CLAWD_ENABLE_ANTIGRAVITY_MONITOR, CLAWD_ENABLED_AGENTS: "antigravity",
      }), CLAWD_SELECTED_AGENT_ID === "antigravity" && CLAWD_ENABLE_ANTIGRAVITY_MONITOR === "true");
    }
  }
});

test("publication replaces the previous snapshot and bubble without leaving session files", (t) => {
  const { root } = fixture(t);
  const stateDir = path.join(root, "state");
  const speechDir = path.join(root, "speech");
  fs.mkdirSync(speechDir);
  const publisher = new LocalStatePublisher({ localStateDir: stateDir, selectedSource: "antigravity" });
  publisher._localSpeechDir = speechDir;
  for (const [sessionId, record] of [["old", answer("old answer")], ["new", prompt("new prompt")]]) {
    const { title, content, state, event } = parseAntigravityStep(JSON.stringify(record));
    publishAntigravityState(publisher, sessionId, state, event, {
      sessionTitle: title, display: { title, content, status: state },
    }, 100);
  }
  assert.deepEqual(fs.readdirSync(stateDir), ["antigravity.json"]);
  assert.deepEqual(fs.readdirSync(speechDir), ["antigravity.json"]);
  const state = JSON.parse(fs.readFileSync(path.join(stateDir, "antigravity.json"), "utf8"));
  const speech = JSON.parse(fs.readFileSync(path.join(speechDir, "antigravity.json"), "utf8"));
  assert.equal(state.sessionId, "new");
  assert.equal(state.state, "working");
  assert.equal(speech.source, state.source);
  assert.equal(speech.displayContent, state.display.content);
  assert.equal(speech.expiresAtMs, 30_100);
});

test("the existing worker wrapper starts and stops the read-only monitor", { timeout: 6_000 }, async (t) => {
  const { brainDir, transcript } = fixture(t);
  const filename = transcript("worker-session");
  // Track this transcript during the initial scan so the append is identified
  // by its contents even if the filesystem timestamp predates worker startup.
  fs.writeFileSync(filename, jsonl(prompt("history")));
  const worker = new Worker(path.join(__dirname, "../agents/log-monitor-worker.js"), {
    workerData: { kind: "antigravity", config: { brainDir } },
  });
  t.after(() => worker.terminate());
  let inputs = 0;
  await new Promise((resolve, reject) => {
    worker.on("error", reject);
    worker.on("message", (message) => {
      if (message.type === "ready") fs.appendFileSync(filename, jsonl(prompt("worker")));
      if (message.event !== "antigravity:user_input") return;
      try {
        assert.equal(message.sessionId, "worker-session");
        assert.equal(message.extra.display.title, "worker");
        assert.equal(message.extra.display.content, "正在思考");
        inputs++;
        worker.postMessage({ type: "stop" });
      } catch (error) {
        reject(error);
      }
    });
    worker.once("exit", (code) => code === 0 ? resolve() : reject(new Error("worker exit " + code)));
  });
  assert.equal(inputs, 1);
});

test("identical replies do not hide a new turn when both records arrive in one poll", (t) => {
  const { transcript, monitor, events } = fixture(t);
  const file = transcript("repeat");
  const repeated = answer("same reply");
  fs.writeFileSync(file, jsonl(prompt("first"), repeated));
  monitor.poll();
  fs.appendFileSync(file, jsonl(prompt("second"), repeated));
  monitor.poll();
  assert.deepEqual(events.slice(1).map(({ state, title }) => [state, title]), [
    ["working", "second"],
    ["done", "second"],
  ]);
});
