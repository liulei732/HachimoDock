"use strict";
const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const Claude = require("../agents/claude-log-monitor");
const Codex = require("../agents/codex-log-monitor");

for (const Monitor of [Claude, Codex]) {
  test(Monitor.name + " preserves split UTF-8 and observes truncate and same-size replacement once", t => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), "session-tail-"));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    const name = "rollout-date-12345678-1234-1234-1234-123456789abc.jsonl";
    const file = path.join(root, name);
    const monitor = new Monitor({ INITIAL_TAIL_BYTES: 0 }, () => {});
    const lines = [];
    monitor.processLine = line => lines.push(JSON.parse(line));
    const poll = () => monitor.pollFile(file, name);
    const bytes = Buffer.from(JSON.stringify("中文任务") + "\n");
    fs.writeFileSync(file, bytes.subarray(0, 2));
    poll();
    fs.appendFileSync(file, bytes.subarray(2));
    poll();
    fs.writeFileSync(file, '"a"\n');
    poll();
    fs.writeFileSync(file + ".new", '"b"\n');
    fs.renameSync(file + ".new", file);
    poll();
    poll();
    assert.deepEqual(lines, ["中文任务", "a", "b"]);
  });
}

test("short reads preserve every byte of a long record and the next line", t => {
  const { readLines } = require("../agents/session-tail");
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "session-short-read-"));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const file = path.join(root, "test.jsonl");
  const long = "字".repeat(30000);
  fs.writeFileSync(file, long + "\nnext\n");
  const read = fs.readSync;
  t.mock.method(fs, "readSync", (fd, buffer, offset, length, position) =>
    read(fd, buffer, offset, Math.min(length, 509), position));
  const cursor = { offset: 0 };
  const lines = [];
  const poll = () => readLines(file, fs.statSync(file), cursor, () => assert.fail(), line => lines.push(line));
  poll();
  poll();
  assert.equal(cursor.offset, fs.statSync(file).size);
  assert.deepEqual(lines, [long, "next"]);
});

test("same-inode rewrite, bounded catch-up and recovery after oversized records", t => {
  const { readLines } = require("../agents/session-tail");
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "session-regrow-"));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const file = path.join(root, "events.jsonl");
  let cursor = { offset: 0 };
  const lines = [];
  const poll = () => readLines(file, fs.statSync(file), cursor,
    () => (cursor = { offset: 0 }), line => lines.push(line));
  fs.writeFileSync(file, "first\n");
  poll();
  fs.writeFileSync(file, "other\n");
  fs.utimesSync(file, new Date(0), new Date(0));
  poll();
  fs.appendFileSync(file, Buffer.alloc(3 * 1024 * 1024, 65));
  poll();
  assert.ok(cursor.offset < fs.statSync(file).size);
  while (cursor.offset < fs.statSync(file).size) poll();
  assert.equal(cursor.fragmentBytes, 0);
  assert.equal(cursor.oversizeLines, 1);
  fs.appendFileSync(file, "\ndone\n");
  poll();
  assert.deepEqual(lines, ["first", "other", "done"]);
});
