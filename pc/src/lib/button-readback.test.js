import assert from "node:assert/strict";
import test from "node:test";
import {requireButtonReadback} from "./button-readback.js";

test("ordinary input sync requires an identified, complete and unambiguous device map",()=>{
  const bindings=[{event:"button.sw3.short_press",action:"page_back",value:""}];
  const state={ok:true,boardDeviceId:"board",config:{bindings},bindingCount:1};
  assert.doesNotThrow(()=>requireButtonReadback(state,"board"));
  for(const change of [{boardDeviceId:"old"},{ok:false},{bindingCount:2},{config:{}},
    {config:{bindings:[]}}, {config:{bindings:[...bindings,...bindings]},bindingCount:2}]) {
    assert.throws(()=>requireButtonReadback({...state,...change},"board"));
  }
});
