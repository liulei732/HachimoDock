import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import test from "node:test";

const keyCode = {"lab.left":1, "lab.right":2, "lab.up":3, "lab.down":4, "lab.a":5, "lab.b":6};

function open(id, saves = {}, seed = 8) {
  const root = new URL("../../builtin-clawpkgs/" + id + "/", import.meta.url);
  const read = name => readFileSync(new URL(name, root));
  const bytes = read("runtime/app.wasm");
  const widget = JSON.parse(read("runtime/widget.json"));
  assert.equal(createHash("sha256").update(bytes).digest("hex"), widget.hachiWasm);
  const buttons = JSON.parse(read("buttons.json"));
  assert.equal(JSON.parse(read("component.json")).id, id);
  assert.ok(buttons.every(b => !b.event.startsWith("button.sw3.")));
  const tones = [], writes = [];
  let texts = [], sprites = [], rects = [];
  const {exports:e} = new WebAssembly.Instance(new WebAssembly.Module(bytes), {hachi:{
    load:(slot, fallback) => saves[slot] ?? fallback,
    save(slot, value) {saves[slot] = value; writes.push(slot);},
    tone:(hz, ms, volume) => tones.push([hz, ms, volume]),
    canvas640(){}, random_u32:() => (seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0),
    rect:(...args) => rects.push(args), sprite:(...args) => sprites.push(args),
    text:(x,y,px,rgb,ptr,n) => texts.push({x,y,value:Buffer.from(e.memory.buffer,ptr,n).toString()}),
  }});
  const event = (type,x,y=0) => e.hachi_event(type,x,y);
  e.hachi_init();
  return {event, tones, saves, writes,
    frame(count=1) {
      for(let i=0;i<count;i++) {texts=[];sprites=[];rects=[];e.hachi_frame(33);}
      return {texts,sprites,rects};
    },
    key(name) {
      const binding = buttons.find(b => b.event === name);
      assert.ok(binding, name); event(4,keyCode[binding.action]);
    },
    tap(x,y) {event(1,x,y);event(3,x,y);},
  };
}

const play = "button.sw1.short_press", secondary = "button.sw2.short_press", left = "knob.rotate_ccw";
const right = "knob.rotate_cw", down = "joystick.down";

test("piano repeats separate strikes, deduplicates drags and restores the saved octave", () => {
  const g = open("wasm-piano");
  g.key(play);g.key(play);g.tap(24,130);g.tap(24,130);
  assert.deepEqual(g.tones.map(t=>t[0]),[262,262,262,262]);
  g.event(1,26,140);g.event(2,27,140);g.event(2,59,140);g.event(2,60,141);
  g.event(2,4,80);g.event(2,60,141);g.event(6,0);g.event(2,25,140);
  assert.deepEqual(g.tones.slice(4).map(t=>t[0]),[262,294,294]);
  g.key(right);g.key(play);g.key(secondary);assert.equal(g.tones.at(-1)[0],660);
  const restored=open("wasm-piano",g.saves);restored.key(play);assert.equal(restored.tones[0][0],524);
});

test("2048 merges once, undo restores board/steps/RNG, keeps record and cancels restart", () => {
  const old={0:0x1111,1:0,2:0,3:0,4:12,5:12,6:42,7:7};
  const g=open("wasm-2046",{...old}); assert.deepEqual(g.writes,[]);
  g.key(left);const next={...g.saves};assert.equal(next[4],20);assert.equal(next[5],20);
  assert.equal(next[0]&255,0x22);assert.equal(next[7],11);
  g.key(play);g.key(secondary);assert.deepEqual(g.saves,next);
  g.tap(570,444);assert.deepEqual(g.saves,{...old,5:20});
  const n=g.writes.length;g.key(secondary);assert.equal(g.writes.length,n);
  g.key(left);assert.deepEqual(g.saves,next);
  const resumed=open("wasm-2046",{...next});resumed.key(secondary);assert.deepEqual(resumed.writes,[]);
  resumed.key(play);resumed.key(play);assert.equal(resumed.saves[4],0);assert.equal(resumed.saves[5],20);
});

test("xiangqi sidebar can undo and request a hint by key; local turn survives restart",()=>{
  const g=open("xiangqi"); g.key(down);g.key(down);g.key(play);g.frame(8);
  const start={...g.saves};
  g.tap(19,154);g.tap(43,154);assert.deepEqual(g.saves,start); // soldier cannot move sideways
  g.tap(19,131);g.frame(8);assert.equal((g.saves[7]>>>16)&1,1);
  const moved={...g.saves};const restored=open("xiangqi",{...moved});restored.frame(8);
  assert.deepEqual(restored.writes,[]);
  for(let n=0;n<9;n++)g.key(right);
  assert.ok(g.frame().rects.some(r=>r[0]===238&&r[1]===137));
  g.key(play);g.frame(8);assert.deepEqual(g.saves,start);
  g.key(right);g.key(down);g.key(play);const hint=g.frame(240);
  assert.ok(hint.rects.some(r=>r[4]===0x397456));assert.deepEqual(g.saves,start);
});

test("pop stars scores connected groups, rejects singles, settles, then starts another round",()=>{
  const g=open("cozy-pop-stars");
  const text=(frame,x,y)=>frame.texts.find(t=>t.x===x&&t.y===y)?.value;
  let frame=g.frame(20);
  const score=()=>Number(text(frame,445,139));
  const tiles=()=>frame.sprites.filter(s=>s[2]===42&&s[3]===42);
  assert.equal(score(),0);assert.equal(tiles().length,56);
  g.key(play);frame=g.frame(30);assert.equal(score(),20);assert.equal(tiles().length,54);
  g.key(play);frame=g.frame(30);assert.equal(score(),20);
  let rounds=1;
  for(;rounds<=28;rounds++){
    const board=tiles();
    const group=board.find(([x,y,w,h,p])=>board.some(t=>t[4]===p&&Math.abs(t[0]-x)+Math.abs(t[1]-y)===47));
    if(!group)break;
    const prior=score();
    g.tap(group[0]+20,group[1]+20);frame=g.frame(30);
    if(score()===prior){g.key(play);frame=g.frame(30);}
    assert.ok(score()>prior || frame.texts.some(t=>/结算|再来一局/.test(t.value)));
  }
  assert.ok(rounds>1&&rounds<=28);
  assert.ok(frame.texts.some(t=>/结算|再来一局/.test(t.value)),JSON.stringify(frame.texts));
  g.key(secondary);frame=g.frame(30);assert.equal(score(),0);assert.equal(tiles().length,56);
});

test("both computer levels reply and undo the player's full turn",()=>{
  for (let level=0;level<2;level++) {
    const g=open("xiangqi");if(level)g.key(down);
    g.key(play);g.frame(8);const initial={...g.saves};
    g.tap(19,154);g.tap(19,131);assert.equal((g.saves[7]>>>16)&1,1);
    g.frame(240);assert.equal((g.saves[7]>>>16)&1,0);
    g.tap(270,150);assert.deepEqual(g.saves,initial);
  }
});

test("2048 handles a full blocked legacy board and the 2048 milestone",()=>{
  const full={0:0x2121,1:0x1212,2:0x2121,3:0x1212,7:1};
  const g=open("wasm-2046",full);assert.ok(g.frame().texts.some(t=>t.value==="没有可移动方向"));
  for(const k of [left,right,"joystick.up",down,secondary])g.key(k);
  assert.deepEqual(g.writes,[]);
  const near=open("wasm-2046",{0:0xaa,4:0,7:1});near.key(left);
  assert.equal(near.saves[4],2048);assert.ok(near.frame(6).texts.some(t=>t.value==="2048 达标"));
});
