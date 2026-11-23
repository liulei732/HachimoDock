import test from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

const runtime = new URL("../../skills/petui/runtime/python/", import.meta.url);
const runner = fileURLToPath(new URL("runner.mjs", runtime));
const run = (source, input, timeout = 3000) => spawnSync(process.execPath, [runner], {
  input: JSON.stringify({ source, input }), encoding: "utf8", timeout,
});

test("the actual Python interpreter needs no host imports and processes Unicode JSON", () => {
  const binary = new WebAssembly.Module(readFileSync(new URL("python.wasm", runtime)));
  assert.deepEqual(WebAssembly.Module.imports(binary), []);
  const result = run("import json\ndef snapshot(data):\n return {'名称':json.loads(data)['名称'], 'total':sum([1,2,3])}", '{"名称":"测试"}');
  assert.equal(result.status, 0);
  assert.deepEqual(JSON.parse(result.stdout), { "名称": "测试", total: 6 });
});

test("the interpreter rejects files and OS modules and discards plugin output on errors", () => {
  const attempts = [
    "def snapshot(data):\n return {'data':open(data).read()}",
    "import os\ndef snapshot(data):\n return {'data':os.environ}",
    "import socket\ndef snapshot(data):\n return {}",
    "import subprocess\ndef snapshot(data):\n return {}",
    "import js\ndef snapshot(data):\n return {}",
    "def snapshot(data):\n raise ValueError(data)",
    "def snapshot(data):\n return {'oversize':'x' * 20000}",
  ];
  for (const source of attempts) {
    const result = run(source, "private-marker");
    assert.equal(result.status, 1);
    assert.equal(result.stdout, "");
    assert.equal(result.stderr, "");
  }
});

test("a runaway Python job can be terminated by its process deadline", () => {
  const result = run("def snapshot(data):\n while True:\n  pass", null, 750);
  assert.equal(result.error?.code, "ETIMEDOUT");
  assert.equal(result.stdout, "");
});
