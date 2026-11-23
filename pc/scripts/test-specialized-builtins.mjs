// Read-only regression of the four specialized builtins. No library or device IO.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
import {deflateSync,crc32} from 'node:zlib';
import {createRuntime} from '../src/component-center/wasm-preview/runtime.mjs';
import {createWasmFrameRenderer} from '../src/component-center/wasm-preview/frame.mjs';
import {loadPreviewFonts} from './wasm-preview-font.mjs';
import {createClassicCore} from '../src/component-center/classic-core.js';
import {initialApp,appDashboard} from '../src/component-center/app-runtime.js';
const pc=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const output=process.argv[2]&&path.resolve(process.argv[2]);
if(output)fs.mkdirSync(output,{recursive:true});
const read=(id,name)=>JSON.parse(fs.readFileSync(path.join(pc,'builtin-clawpkgs',id,name),'utf8'));
const bytes=id=>fs.readFileSync(path.join(pc,'builtin-clawpkgs',id,'runtime/app.wasm'));
const hash=frame=>createHash('sha256').update(JSON.stringify(frame.pixels)).update(JSON.stringify(frame.texts)).digest('hex');
const renderer=output?createWasmFrameRenderer(await loadPreviewFonts()):null;
function chunk(kind,data){const type=Buffer.from(kind),size=Buffer.alloc(4),crc=Buffer.alloc(4);size.writeUInt32BE(data.length);crc.writeUInt32BE(crc32(Buffer.concat([type,data])));return Buffer.concat([size,type,data,crc]);}
function capture(name,frame){
 if(!output)return;
 let rgba;const context={createImageData:(width,height)=>({width,height,data:new Uint8ClampedArray(width*height*4)}),putImageData:image=>{rgba=image.data;}};
 const canvas={width:640,height:480,getContext:()=>context};renderer(canvas,{...frame,init:true});
 const header=Buffer.alloc(13);header.writeUInt32BE(640);header.writeUInt32BE(480,4);header[8]=8;header[9]=6;
 const rows=Buffer.alloc(480*(640*4+1));for(let y=0;y<480;y++)rows.set(rgba.subarray(y*640*4,(y+1)*640*4),y*(640*4+1)+1);
 fs.writeFileSync(path.join(output,`${name}.png`),Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]),chunk('IHDR',header),chunk('IDAT',deflateSync(rows)),chunk('IEND',Buffer.alloc(0))]));
}
async function guest(id,save={}){
 const run=await createRuntime(bytes(id),save);run('init',[]);const trace=[],slots={...save};let frames=0,peakText=0,peakDraws=0;
 function frame(){trace.push('frame 16');frames++;const f=run('frame',[16]);peakText=Math.max(peakText,f.texts.length);peakDraws=Math.max(peakDraws,f.pixels.length);return f;}
 function key(key){trace.push(`event 4 ${key} 0`);const result=run('event',[4,key,0]);for(const [slot,value]of result.saves)slots[slot]=value;return frame();}
 return {run,frame,key,trace,slots,stats:()=>({frames,peakText,peakDraws})};
}
const result=[];
for(const id of ['chengyu-jian','today-meal-menu','inkline-gomoku','fresh-fruit-slice']){
 const c=read(id,'component.json'),buttons=read(id,'buttons.json');assert.equal(c.id,id);assert.match(c.version,/^\d+\.\d+\.\d+$/);
 assert(!buttons.some(b=>b.event==='button.sw3.short_press'||/^page_/.test(b.action)),'global exit remains independent');
}
{
 const id='chengyu-jian',g=await guest(id);capture(`${id}-ready`,g.frame());
 const size=g.key(6);assert(size.texts.some(x=>x.value==='大笺'));assert.equal(g.slots[2],2);
 const color=g.key(4);assert(color.texts.some(x=>x.value==='竹青'));assert.equal(g.slots[1],1);
 let f=g.key(5);capture(`${id}-playing`,f);
 for(const key of [2,1]){const before=hash(f);f=g.key(key);assert.notEqual(hash(f),before,'candidate selection must visibly change');}
 let attempts=0,wrong=0;const completed=new Set();
 for(;attempts<4000;attempts++){
  const text=f.texts.map(x=>x.value);
  if(text.includes('三关圆满'))break;
  if(text.includes('答对了')){completed.add(g.slots[0]);f=g.key(5);continue;}
  f=g.key(5);
  if(f.texts.some(x=>x.value==='再想一想，换个字试试')){wrong++;f=g.key(2);}
 }
 assert(attempts<4000,'all 80 cards must be completable');assert.equal(g.slots[0],80);assert.equal(completed.size,80);assert(wrong>0);
 capture(`${id}-completed`,f);g.trace.push('text 三关圆满');
 if(output)fs.writeFileSync(path.join(output,`${id}.trace`),g.trace.join('\n')+'\n');
 const replay=g.key(5);assert(replay.texts.some(x=>x.value==='01 / 20'));assert.equal(g.slots[0],0);
 const restored=await guest(id,{0:79,1:1,2:2});const old=restored.key(5);
 assert(old.texts.some(x=>x.value==='30 / 30'));assert(old.texts.some(x=>x.value==='大笺'));assert(old.texts.some(x=>x.value==='竹青'));
 for(const progress of [-1,2147483647]){const corrupt=await guest(id,{0:progress});assert(corrupt.key(5).texts.some(x=>x.value==='01 / 20'));}
 result.push({id,ok:true,completed:completed.size,attempts,wrongFeedback:wrong,saveSlots:[0,1,2],restoredFinalCard:true,restart:true,...g.stats()});
}
{
 const id='today-meal-menu',g=await guest(id);capture(`${id}-ready`,g.frame());
 // Check each directional binding in a state where selection can move.
 for(const key of [2,1,4,3]){const before=hash(g.frame());assert.notEqual(hash(g.key(key)),before);}
 const categories=['放纵一下','自律达人','健身达人','省心选手'];const menus=new Set();
 for(let person=0;person<4;person++){
  let f=g.key(5);assert.equal(f.texts[0].value,categories[person]);
  for(let menu=0;menu<6;menu++){
   const text=f.texts.map(x=>x.value);assert(text.includes(String(menu+1).padStart(2,'0')));for(const meal of ['早餐','午餐','晚餐'])assert(text.includes(meal));
   const dishes=f.texts.filter(x=>x.x===179&&x.px===25).map(x=>x.value);assert.equal(dishes.length,3);menus.add(dishes.join('|'));
   if(person===0&&menu===0)capture(`${id}-menu`,f);
   f=g.key(5);
  }
  assert(f.texts.some(x=>x.value==='01'),'six menus must wrap to first');
  assert(g.key(6).texts.some(x=>x.value==='今天，想做哪种自己？'),'secondary returns to selection');
  if(person===0||person===2)g.key(2);
  else if(person===1){g.key(1);g.key(4);}
 }
 assert.equal(menus.size,24);if(output)fs.writeFileSync(path.join(output,`${id}.trace`),g.trace.join('\n')+'\n');
 result.push({id,ok:true,categories:4,distinctMenus:menus.size,threeMealsEach:true,directionalBindings:4,returnToSelection:true,...g.stats()});
}
{
 const id='inkline-gomoku',runtime=read(id,'runtime/widget.json');
 const g=await createClassicCore(fs.readFileSync(path.join(pc,'src/component-center/classic-core.wasm')),runtime),actions=runtime.game.actions;
 g.action(actions.place,1);assert(g.snapshot().running);g.action(actions.place,2);assert.equal(g.snapshot().score,1);
 g.action(actions.place,3);assert.equal(g.snapshot().score,1,'white pending must block duplicate placement');
 g.action(actions.undo,4);assert.equal(g.snapshot().score,0,'undo pending player stone');
 g.action(actions.place,5);g.tick(255);assert.equal(g.snapshot().score,2);g.action(actions.place,256);assert.equal(g.snapshot().score,2,'occupied cell must reject placement');
 g.action(actions.undo,257);assert.equal(g.snapshot().score,0);const before=g.snapshot();g.action(actions.undo,258);assert.deepEqual(g.snapshot(),before,'only one round can be undone');
 const headings=[];for(const state of runtime.states){const app={...initialApp(runtime),state};headings.push(appDashboard(runtime,app).headline);}
 assert.equal(new Set(headings).size,3,'fallback must distinguish ready, playing and result');
 result.push({id,ok:true,pendingUndo:true,replyUndo:true,duplicatePlaceBlocked:true,occupiedPlaceBlocked:true,oneRoundUndo:true,fallbackHeadlines:headings,aiReads:g.snapshot().aiReads});
}
console.log(JSON.stringify({ok:true,deviceVerified:false,results:result},null,2));
