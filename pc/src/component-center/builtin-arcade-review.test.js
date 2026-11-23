import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createSceneCore } from "./scene-core.js";
import { createClassicCore } from "./classic-core.js";
import { appAction, appDashboard, appVisible, initialApp } from "./app-runtime.js";

const wasm = readFileSync(new URL("./classic-core.wasm", import.meta.url));
const read = (id, file) => JSON.parse(readFileSync(new URL(`../../builtin-clawpkgs/${id}/${file}`, import.meta.url)));

test("the authoring tool ships the same classic engine as the desktop preview", () => {
  assert.deepEqual(readFileSync(new URL("../../skills/petui/scripts/classic-core.wasm", import.meta.url)), wasm);
});

test("the desktop binary rotates a bar flush with each wall", async () => {
  const runtime = read("block-combo", "runtime/widget.json");
  const { actions } = runtime.game;
  for (const side of ["left", "right"]) {
    const core = await createClassicCore(wasm, runtime);
    core.action(actions.start, 0);
    core.action(actions.rotate, 1);
    const { width } = core.snapshot();
    for (let i = 0; i < width; i++) core.action(actions[side], 2 + i);
    const x = side === "left" ? 0 : width - 1;
    assert.equal(core.snapshot().cells.filter((value, i) => value && i % width === x).length, 4);
    core.action(actions.rotate, width + 2);
    const start = width + (side === "left" ? 0 : width - 4);
    assert.deepEqual(core.snapshot().cells.slice(start, start + 4), [1, 1, 1, 1]);
  }
});

test("playing a full snake board leaves one head, 159 body cells and no food", async () => {
  const runtime = read("snake-turn", "runtime/widget.json");
  const { actions, tick_ms } = runtime.game;
  const core = await createClassicCore(wasm, runtime);
  core.action(actions.start, 0);
  let frame = core.snapshot();
  const { width, height } = frame;
  const capacity = width * height;
  // Sweep alternating rows, leaving the last column for the return to the top.
  for (let tick = 1; !frame.over && tick <= capacity * capacity; tick++) {
    const head = frame.cells.indexOf(3);
    assert.ok(head >= 0);
    const x = head % width;
    const y = Math.floor(head / width);
    let direction;
    if (x === width - 1) direction = y ? "up" : "left";
    else if (y % 2 === 0) direction = x ? "left" : "down";
    else direction = x === width - 2 && y !== height - 1 ? "down" : "right";
    core.action(actions[direction], tick * tick_ms - 1);
    core.tick(tick * tick_ms);
    frame = core.snapshot();
  }
  assert.equal(frame.state, runtime.game.result_state);
  assert.equal(frame.score, 156);
  assert.equal(frame.cells.filter(c => c === 1).length, 159);
  assert.equal(frame.cells.filter(c => c === 3).length, 1);
  assert.ok(!frame.cells.includes(2));
});

test("flappy crosses a real gate and leaves the bird visible on a floor hit", async () => {
  const runtime = read("flappy-bird", "runtime/widget.json");
  const { actions, tick_ms } = runtime.game;
  const core = await createClassicCore(wasm, runtime);
  core.action(actions.flap, 0);
  let now = 0;
  for (let i = 0; i < 25 && !core.snapshot().score; i++) {
    const frame = core.snapshot();
    if (Math.floor(frame.cells.indexOf(3) / frame.width) >= 6) core.action(actions.flap, now + 1);
    now += tick_ms;
    core.tick(now);
  }
  assert.equal(core.snapshot().score, 1);
  for (let i = 0; i < 20 && !core.snapshot().over; i++) {
    now += tick_ms;
    core.tick(now);
  }
  const frame = core.snapshot();
  assert.equal(frame.state, runtime.game.result_state);
  assert.equal(frame.score, 1);
  assert.ok(frame.cells.slice(-frame.width).includes(3));
});

test("one paddle contact scores once even on a burst of controls before the next tick", async () => {
  const runtime = read("two-key-pong", "runtime/widget.json");
  const core = await createSceneCore(wasm, runtime.scene);
  const entity = id => core.snapshot().entities.find(e => e.id === id);
  core.action("start", 1);
  let now = 1;
  for (let i = 0; i < 30 && !core.snapshot().score; ++i) {
    core.pointer("bat", entity("orb").x + 1, 0, 1);
    now += runtime.scene.tick_ms;
    core.tick(now);
  }
  assert.equal(core.snapshot().score, 1);
  const contactY = entity("orb").y;
  assert.equal(contactY + entity("orb").height, entity("bat").y);
  for (let i = 0; i < 10; ++i) {
    core.action("shift_left", now);
    core.action("shift_right", now);
  }
  assert.equal(core.snapshot().score, 1);
  now += runtime.scene.tick_ms;
  core.tick(now);
  assert.ok(entity("orb").y < contactY);
  for (let i = 0; i < 100 && !core.snapshot().over; ++i) {
    core.pointer("bat", entity("orb").x < 8 ? 15 : 0, 0, 1);
    now += runtime.scene.tick_ms;
    core.tick(now);
  }
  assert.ok(core.snapshot().over);
  core.action("start", now + 1);
  assert.equal(core.snapshot().score, 0);
  assert.ok(core.snapshot().running);
});

test("cloud jump HUD reports actual held charge and offers pause only while usable", async () => {
  const runtime = read("cloud-hop", "runtime/widget.json");
  const core = await createSceneCore(wasm, runtime.scene);
  let state = initialApp(runtime);
  const visible = node => appVisible(node, 0, state.vars, runtime.states.indexOf(state.state));
  const nodes = runtime.surface.elements;
  const pause = nodes.find(n => n.action === "session.pause");
  assert.equal(visible(pause), false);
  state = appAction(runtime, state, runtime.input.hold);
  core.action(runtime.input.hold, 100);
  let previous = -1;
  for (let elapsed = 0; elapsed <= 1200; elapsed += 20) {
    core.tick(100 + elapsed);
    state.vars.charge = core.snapshot().charge;
    const number = nodes.find(n => visible(n) && n.bind === "headline");
    assert.equal(Number(appDashboard(runtime, state)[number.bind]), state.vars.charge);
    assert.ok(state.vars.charge >= previous);
    previous = state.vars.charge;
  }
  assert.equal(previous, 100);
  assert.ok(nodes.some(n => visible(n) && n.text === "%"));
  core.release(false, 1300);
  core.tick(1320);
  assert.equal(core.snapshot().charge, 0);
  assert.equal(core.snapshot().grounded, false);
  assert.ok(visible(pause));
  state = appAction(runtime, state, pause.action);
  assert.ok(visible(pause));
  assert.ok(!nodes.some(n => visible(n) && n.text === "%"));
  state = appAction(runtime, state, pause.action);
  assert.equal(state.state, "playing");
  for (let tick = 1; tick <= 120 && !core.snapshot().grounded && !core.snapshot().over; tick++) {
    core.tick(1320 + tick * 20);
  }
  assert.ok(core.snapshot().grounded || core.snapshot().over);
  state.state = runtime.scene.result_state;
  assert.ok(!visible(pause));
});

test("five factory games retain their record slot and leave the device exit binding available", () => {
  for (const id of ["block-combo","snake-turn","flappy-bird","two-key-pong","cloud-hop"]) {
    assert.equal(read(id, "component.json").id, id);
    assert.deepEqual(read(id, "hachiapp.json"), { format: "hachiapp/1", id });
    const runtime = read(id, "runtime/widget.json");
    assert.equal(runtime.persist.version, 1);
    assert.deepEqual(runtime.persist.vars, ["best"]);
    assert.equal(runtime.lifecycle.record_var, "best");
    assert.ok(runtime.states.every(s => runtime.persist.states[s] === "ready"));
    assert.ok(read(id, "buttons.json").every(b => !/sw3|page_main|page_back/i.test(b.event + b.action)));
  }
});
