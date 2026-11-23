import test from "node:test";
import assert from "node:assert/strict";
import {pollWhileVisible} from "./visible-poll.js";

test("tray suspends display reads and restores one timer with an immediate refresh", () => {
  const page = new EventTarget();
  const jobs = new Map();
  let id = 0, reads = 0;
  const clock = {
    setInterval(fn, ms) { jobs.set(++id, {fn, ms}); return id; },
    clearInterval(key) { jobs.delete(key); },
  };
  const stop = pollWhileVisible(() => ++reads, 2000, page, clock);
  assert.equal(jobs.size, 1);
  page.visibilityState = "hidden";
  page.dispatchEvent(new Event("visibilitychange"));
  for (const job of jobs.values()) for (let t = 0; t < 1800000; t += job.ms) job.fn();
  assert.equal(reads, 0);
  page.visibilityState = "visible";
  page.dispatchEvent(new Event("visibilitychange"));
  assert.equal(reads, 1);
  assert.equal(jobs.size, 1);
  stop();
  page.dispatchEvent(new Event("visibilitychange"));
  assert.equal(jobs.size, 0);
  assert.equal(reads, 1);
});
