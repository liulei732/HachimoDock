import assert from "node:assert/strict";
import test from "node:test";
import {execFileSync} from "node:child_process";
import {createHash} from "node:crypto";
import {readFileSync} from "node:fs";
import {fileURLToPath} from "node:url";
import {appAction,appAdvance,appVisible,initialApp} from "../../src/component-center/app-runtime.js";

function entry(id, path) {
  const zip = id === "upgrade-fixture" ? process.env.PETUI_UPGRADE_SHELF :
    fileURLToPath(new URL(`packages/${id}.hachiapp`,import.meta.url));
  return execFileSync(process.platform==="win32"?"python":"python3",["-c",
    "import sys,zipfile;sys.stdout.buffer.write(zipfile.ZipFile(sys.argv[1]).read(sys.argv[2]))",zip,path]);
}
const modules=new Map();
function launch(id, slots={}, clock=null) {
  if (!modules.has(id)) {
    const bytes=entry(id,"runtime/app.wasm");
    assert.equal(JSON.parse(entry(id,"runtime/widget.json")).hachiWasm,createHash("sha256").update(bytes).digest("hex"));
    modules.set(id,new WebAssembly.Module(bytes));
  }
  let texts=[],tiles=[],writes=0;
  const {exports:e}=new WebAssembly.Instance(modules.get(id),{hachi:{
    rect(){},canvas640(){},random_u32:()=>12345678,
    load:(n,defaultValue)=>slots[n]??defaultValue,save(n,v){slots[n]=v;writes++;},
    clock_read(kind,p){
      if (!clock) return 0;
      const d=new DataView(e.memory.buffer);
      d.setBigUint64(p,BigInt(clock.ms),true); d.setInt32(p+8,clock.offset,true);return clock.status??1;
    },
    sprite:(x,y,_w,_h,art)=>tiles.push([x,y,art]),
    text:(_x,_y,_px,_rgb,p,n)=>texts.push(Buffer.from(e.memory.buffer,p,n).toString()),
  }});
  function frame(){texts=[];tiles=[];e.hachi_frame(33);return [...texts];}
  e.hachi_init();
  return {frame, key:k=>e.hachi_event(4,k,0),tap:(x,y)=>{e.hachi_event(1,x,y);e.hachi_event(3,x,y);},
    get tiles(){return tiles;},get writes(){return writes;}};
}
for (const [id,title] of [["book-pocket-offline","书袋 · 离线精选"],["hachimi-learn-offline","知识 · 离线精选"]]) {
  test(id+" restores the selected article and page across a new instance",()=>{
    const slots={};const g=launch(id,slots);
    assert.equal(JSON.parse(entry(id,"runtime/widget.json")).offlineReading,"embedded-shelf-v1");
    assert.ok(g.frame().includes(title));g.key(4);g.key(5);
    const first=g.frame();g.key(2);const second=g.frame();
    assert.notDeepEqual(first,second);assert.equal(slots[1],1);
    const after=launch(id,slots);
    assert.deepEqual(after.frame(),second);assert.equal(after.writes,0);
    after.key(6);assert.ok(after.frame().includes(title));
    assert.deepEqual(launch(id,{0:-9,1:100000}).frame(),launch(id).frame());
    const truncated=launch(id,{...slots,1:0x7fff});
    assert.ok(truncated.frame().includes("目录"));
  });
}
// The build/integration run supplies a real replacement shelf with the first
// article moved and the second removed. Both packages use the same device ID.
if (process.env.PETUI_UPGRADE_SHELF) {
  test("replacing a shelf preserves a moved article and handles an omitted one",()=>{
    const oldId="book-pocket-offline", nextId="upgrade-fixture";
    assert.equal(JSON.parse(entry(nextId,"component.json")).id,oldId);
    for (const removed of [false,true]) {
      const slots={}, old=launch(oldId,slots);
      old.frame();
      if (removed) old.key(4);
      old.key(5);old.key(2);
      const saved=old.frame();
      assert.equal(slots[1],1);
      const next=launch(nextId,slots);
      if (removed) assert.deepEqual(next.frame(),launch(nextId).frame());
      else assert.deepEqual(next.frame(),saved);
      assert.equal(next.writes,0);
    }
  });
}
const stars="cozy-pop-stars-daily";
test("daily board, cancellation, power-cycle resume, yesterday and replay",()=>{
  const slots={};const clock={ms:Date.UTC(2026,10,21,16,20),offset:480};
  const g=launch(stars,slots,clock);
  assert.ok(g.frame().includes("每日一局"));g.key(4);g.key(5);g.frame();
  const initial=g.tiles;const seed=slots[0];const day=slots[4];
  assert.equal(day,Math.floor((clock.ms+480*60000)/86400000));
  g.key(5);g.frame();assert.ok(slots[3]>=20);
  const moved=g.tiles;assert.ok(moved.length<56);
  const restored=launch(stars,slots,clock);
  assert.ok(restored.frame().includes("继续上局"));restored.key(5);restored.frame();
  assert.deepEqual(restored.tiles,moved);assert.equal(restored.writes,0);
  restored.key(6);restored.key(4);restored.key(5);
  assert.ok(restored.frame().includes("再确认一次替换上局，副键取消"));
  restored.key(6);restored.frame();assert.deepEqual(restored.tiles,moved);
  for (let i=0;i<28;i++) {
    const board=restored.tiles;
    const pair=board.find(([x,y,p])=>board.some(([a,b,q])=>p===q && Math.abs(a-x)+Math.abs(b-y)===48));
    if (!pair) break;
    const old=slots[3];restored.tap(pair[0]+12,pair[1]+12);
    if (slots[3]===old) restored.key(5);
    assert.ok(slots[3]>old);restored.frame();
  }
  assert.ok(restored.frame().includes("结算"));const best=slots[3];
  restored.key(5);restored.frame();assert.deepEqual(restored.tiles,initial);assert.equal(slots[0],seed);
  const tomorrow=launch(stars,slots,{...clock,ms:clock.ms+86400000});
  assert.ok(tomorrow.frame().includes(String(best)));
  tomorrow.key(4);tomorrow.key(4);tomorrow.key(5);tomorrow.key(5);tomorrow.frame();
  assert.equal(slots[4],day+1);assert.notDeepEqual(tomorrow.tiles,initial);
  const damaged=launch(stars,{...slots,7:slots[7]^4},clock);
  assert.ok(damaged.frame().includes("没有待续棋局"));assert.equal(damaged.writes,0);
});
test("missing or stale wall clock leaves free play available",()=>{
  for (const clock of [null,{ms:Date.now(),offset:0,status:2}]) {
    const slots={};const g=launch(stars,slots,clock);
    assert.ok(g.frame().includes("每日一局 · 等待校时"));
    g.key(4);g.key(5);assert.equal(g.writes,0);
    g.key(3);g.key(5);assert.ok(g.frame().includes("自由一局"));assert.equal(slots[4],0);
  }
});
test("focus is opt-in, expires after four rest ticks, and preserves the original save format",()=>{
  const r=JSON.parse(entry("tomato-clock","runtime/widget.json"));
  const old=JSON.parse(readFileSync(new URL("../../builtin-clawpkgs/tomato-clock/runtime/widget.json",import.meta.url)));
  assert.deepEqual(r.persist,old.persist);
  const greet=r.surface.elements.find(n=>n.show?.var==="pet_greeting" && n.type==="image");
  const show=s=>appVisible(greet,0,s.vars,r.states.indexOf(s.state));
  const complete=s=>appAdvance(r,{...s,vars:{...s.vars,remaining_s:1}},1000);
  for (const enabled of [false,true]) {
    let s=initialApp(r);
    if (enabled) s=appAction(r,s,"tomato.pet_on");
    s=appAction(r,s,"tomato.start_pause");
    const skip=appAction(r,s,"tomato.skip_phase");assert.equal(skip.vars.completed_sessions,0);assert.equal(show(skip),false);
    s=complete(s);assert.equal(s.vars.completed_sessions,1);assert.equal(show(s),enabled);
    assert.equal(show(appAction(r,s,"tomato.pet_off")),false);
    for(let i=0;i<4;i++)s=appAdvance(r,s,1000);
    assert.equal(show(s),false);
  }
});
