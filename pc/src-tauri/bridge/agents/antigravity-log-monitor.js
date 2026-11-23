"use strict";

const { createHash } = require("node:crypto");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { parseAntigravityStep } = require("./antigravity-transcript");

const POLL_MS = 750;
const REFRESH_MS = 10_000;
const DEFAULT_IDLE_CLEAR_MS = 60_000;
const MAX_TRACKED_FILES = 16;
const MAX_TAIL_BYTES = 1024 * 1024;
const MAX_TAIL_LINES = 256;

function fileInfo(filename) {
  return fs.lstatSync(filename, { throwIfNoEntry: false });
}

function version(stat) {
  return `${stat.dev}:${stat.ino}:${stat.size}:${stat.mtimeMs}:${stat.ctimeMs}`;
}

function readTail(filename, stat) {
  const fd = fs.openSync(filename, "r");
  try {
    if (version(fs.fstatSync(fd)) !== version(stat)) return null;
    const length = Math.min(stat.size, MAX_TAIL_BYTES);
    const start = stat.size - length;
    const buffer = Buffer.alloc(length + Number(start > 0));
    const offset = start > 0 ? start - 1 : start;
    if (fs.readSync(fd, buffer, 0, buffer.length, offset) !== buffer.length
      || version(fs.fstatSync(fd)) !== version(stat)) return null;

    const lines = buffer.toString("utf8").split("\n");
    lines.pop(); // Wait for a complete JSONL record, including on UTF-8 splits.
    if (start > 0) lines.shift(); // Drop the preceding byte and any partial first record.
    return lines.slice(-MAX_TAIL_LINES).filter((line) => line.trim()).map((line) => {
      const text = line.replace(/\r$/, "");
      return { text, key: createHash("sha256").update(text).digest("base64") };
    });
  } finally {
    fs.closeSync(fd);
  }
}

function findResumeIndex(previousKeys, keys) {
  if (!previousKeys) return -1;
  const current = new Set(keys);
  const oldIndex = previousKeys.findLastIndex((key) => current.has(key));
  if (oldIndex < 0) return -1;

  // Match the newest surviving record, using neighboring records to disambiguate
  // repeated rows. With equal context, the earliest occurrence leaves new rows
  // for the next turn instead of swallowing them.
  let resume = -1;
  let bestLength = 0;
  for (let index = 0; index < keys.length; index++) {
    let length = 0;
    while (length <= Math.min(index, oldIndex) && keys[index - length] === previousKeys[oldIndex - length]) {
      length++;
    }
    if (length > bestLength) {
      bestLength = length;
      resume = index;
    }
  }
  return resume;
}

class AntigravityLogMonitor {
  constructor({
    brainDir = path.join(process.env.HOME || process.env.USERPROFILE || os.homedir(), ".gemini", "antigravity", "brain"),
    now = Date.now,
    idleClearMs = process.env.CLAWD_ANTIGRAVITY_SPEECH_IDLE_CLEAR_MS,
  }, onState) {
    this.brainDir = brainDir;
    this.now = now;
    this.onState = onState;
    const timeout = idleClearMs == null || idleClearMs === "" ? NaN : Number(idleClearMs);
    this.idleClearMs = Number.isSafeInteger(timeout) && timeout >= 0 ? timeout : DEFAULT_IDLE_CLEAR_MS;
    this.files = new Map();
  }

  start() {
    if (this.timer) return;
    this.poll();
    this.timer = setInterval(() => this.poll(), POLL_MS);
  }

  stop() {
    clearInterval(this.timer);
    this.timer = null;
  }

  listTranscripts() {
    const files = [];
    try {
      if (!fileInfo(this.brainDir)?.isDirectory()) return files;
      for (const entry of fs.readdirSync(this.brainDir, { withFileTypes: true })) {
        if (!entry.isDirectory() || !/^[A-Za-z0-9_-]{1,127}$/.test(entry.name)) continue;
        const generated = path.join(this.brainDir, entry.name, ".system_generated");
        const logs = path.join(generated, "logs");
        const filename = path.join(logs, "transcript.jsonl");
        try {
          if (!fileInfo(generated)?.isDirectory() || !fileInfo(logs)?.isDirectory()) continue;
          const stat = fileInfo(filename);
          if (stat?.isFile()) files.push({ sessionId: entry.name, filename, stat });
        } catch {
          // A conversation may be rotated or removed while listing it.
        }
      }
    } catch {
      // The CLI can be installed before the first conversation is created.
    }
    return files.sort((a, b) => a.stat.mtimeMs - b.stat.mtimeMs).slice(-MAX_TRACKED_FILES);
  }

  poll() {
    const now = this.now();
    const baseline = this.startedAt === undefined;
    this.startedAt ??= now;
    for (const { sessionId, filename, stat } of this.listTranscripts()) {
      const previous = this.files.get(sessionId);
      if (previous?.version === version(stat)) continue;
      let lines;
      try {
        lines = readTail(filename, stat);
      } catch {
        continue;
      }
      if (!lines) continue;

      // Find a complete record shared with the previous view. A trimmed or
      // replaced file can keep the same size and reuse indices and offsets.
      // If no record survives, parse the new generation from its bounded tail.
      const keys = lines.map(({ key }) => key);
      const anchor = findResumeIndex(previous?.keys, keys);
      const historical = baseline || (!previous && stat.mtimeMs < this.startedAt);
      let last = previous?.last;
      for (const { text } of lines.slice(anchor + 1)) {
        const step = parseAntigravityStep(text, last);
        if (!step) continue;
        last = step;
        if (!historical) this.publish({ ...step, sessionId, changedAt: now }, now);
      }
      this.files.delete(sessionId);
      // Keep the cursor through an observed truncate-to-zero or delete/recreate gap.
      this.files.set(sessionId, {
        version: version(stat),
        keys: keys.length ? keys : previous?.keys ?? keys,
        last,
      });
    }
    while (this.files.size > MAX_TRACKED_FILES) this.files.delete(this.files.keys().next().value);

    if (!this.current) {
      this.publish({ state: "idle", event: "antigravity:ready", title: "Antigravity", content: "" }, now);
    } else if (this.idleClearMs > 0 && ["done", "error"].includes(this.current.state)
      && now - this.current.changedAt >= this.idleClearMs) {
      this.publish({ ...this.current, state: "idle", event: "antigravity:idle_clear", content: "" }, now);
    } else if (now - this.lastPublished >= REFRESH_MS) {
      // Renew the existing forwarders' freshness window for Wi-Fi and reconnect.
      this.publish(this.current, now);
    }
  }

  publish(step, now) {
    this.current = step;
    this.lastPublished = now;
    this.onState(step.sessionId, step.state, step.event, {
      sessionTitle: step.title,
      display: { title: step.title, content: step.content, status: step.state },
    });
  }
}

module.exports = AntigravityLogMonitor;
