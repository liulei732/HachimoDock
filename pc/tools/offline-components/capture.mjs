import {readFileSync} from "node:fs";
import {createHash} from "node:crypto";
const module = new WebAssembly.Module(readFileSync(process.argv[2]));
let commands=[];
const {exports:e}=new WebAssembly.Instance(module,{hachi:{
  canvas640(){}, load:(_slot,fallback)=>fallback,save(){},random_u32:()=>8,clock_read:()=>0,
  rect:(...args)=>commands.push(["rect",...args]),
  sprite:(x,y,_w,_h,p,n)=>commands.push(["sprite",x,y,
    createHash("sha256").update(Buffer.from(e.memory.buffer,p,n)).digest("hex")]),
  text:(x,y,px,rgb,p,n)=>commands.push(["text",x,y,px,rgb,Buffer.from(e.memory.buffer,p,n).toString()]),
}});
e.hachi_init(); e.hachi_frame(0); e.hachi_event(4,5,0);
commands=[];e.hachi_frame(33);
process.stdout.write(JSON.stringify(commands));
