import test from "node:test";
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {initialApp, appAction, appAdvance, appDashboard, appHit} from "./app-runtime.js";

function read(id, name) {
  return JSON.parse(readFileSync(new URL(`../../builtin-clawpkgs/${id}/${name}`, import.meta.url)));
}

test("paused focus and rest let the user choose a new duration and keep completed rounds", () => {
  const id = "tomato-clock", r = read(id, "runtime/widget.json");
  const buttons = read(id, "buttons.json");
  const select = buttons.find(b => b.event === "button.encoder.short_press").action;
  const hit = s => appHit(r.surface, 0, 565, 402, s.vars, r.states.indexOf(s.state));
  for (const phase of ["focus","rest"]) {
    let s = appAction(r, initialApp(r), "tomato.start_pause");
    if (phase === "rest") for (let i=0; i<1500; ++i) s=appAdvance(r,s,1000);
    const completed = s.vars.completed_sessions;
    assert.equal(s.state, phase);
    assert.equal(hit(s), null);
    s = appAction(r,s,"tomato.start_pause");
    assert.equal(hit(s),select);
    assert.ok(r.input.confirm[select]);
    s = appAction(r,s,select);
    s = appAction(r,s,"tomato.shorter");
    assert.equal(appDashboard(r,s).metricValue,"20:00");
    s = appAction(r,s,"tomato.start_pause");
    assert.equal(s.vars.remaining_s,1200);
    assert.equal(s.vars.completed_sessions,completed);
  }
});

test("editing the next hydration interval displays its value without changing the active timer", () => {
  const r = read("drink-reminder","runtime/widget.json");
  let s = appAdvance(r,initialApp(r),60000);
  const up = appHit(r.surface,0,560,401,s.vars), down = appHit(r.surface,0,440,400,s.vars);
  assert.equal(up,"reminder.more");
  assert.equal(down,"reminder.less");
  for(let i=0;i<10;i++) s=appAction(r,s,up);
  assert.equal(appDashboard(r,s).badge,"120");
  assert.equal(s.vars.remaining_min,59);
  s=appAction(r,s,"reminder.switch_view");
  assert.equal(appDashboard(r,s).metricValue,"0");
  for(let i=0;i<10;i++) s=appAction(r,s,down);
  assert.equal(appDashboard(r,s).badge,"15");
  assert.equal(s.vars.remaining_min,59);
  s=appAction(r,s,"reminder.acknowledge");
  assert.equal(s.vars.remaining_min,15);
  assert.equal(s.vars.drink_count_today,1);
});
