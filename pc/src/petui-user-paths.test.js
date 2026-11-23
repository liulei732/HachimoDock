// Exercise the production example and clipboard handler with synthetic users.
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
const source=fs.readFileSync(new URL('./ComponentCenter.jsx',import.meta.url),'utf8');
const example=source.match(/const invocationExample = \[[\s\S]*?\]\.join\("\\n"\);/)?.[0];
const copy=source.match(/async function copyInvocationExample\(\) \{[\s\S]*?\n  \}/)?.[0];
assert(example&&copy,'the production drawer and copy handler must be present');
for(const home of ['C:\\Users\\Fixture User','D:\\Profiles\\用户 甲']){
 test(`copied example belongs to ${home}`,async()=>{
  const authoring={skillPath:`${home}\\.agents\\skills\\petui`,command:`${home}\\.claw-pet\\authoring\\petui.cmd`};
  let copied='',ready=false,error='';
  const context={authoring,navigator:{clipboard:{writeText:async text=>{copied=text;}}},setCopyError:value=>{error=value;},setInvocationCopied:value=>{ready=value;},window:{setTimeout:()=>{}},console};
  const rendered=vm.runInNewContext(`${example}\ninvocationExample`,context);
  await vm.runInNewContext(`${copy}\ncopyInvocationExample()`,vm.createContext({...context,invocationExample:rendered}));
  assert.equal(copied,rendered);assert(copied.includes(`PetUI Skill：${authoring.skillPath}`));assert(copied.includes(authoring.command));assert.equal(ready,true);assert.equal(error,'');
  assert(!copied.includes('/Users/')&&!copied.includes('/home/'));
 });
}
test('unprepared drawer uses generic guidance without a stale user',()=>{
 const text=vm.runInNewContext(`${example}\ninvocationExample`,{authoring:{phase:'preparing'}});
 assert(text.includes('用户目录下的 .agents/skills/petui'));assert(!text.includes('C:\\Users\\'));
});
test('native home selector and Windows launcher share the current profile',()=>{
 const lib=fs.readFileSync(new URL('../src-tauri/src/lib.rs',import.meta.url),'utf8');
 assert.match(lib,/mod user_home;/);assert.match(lib,/fn get_home_dir\(\)[\s\S]*?user_home::current\(\)/);
 const prepare=lib.slice(lib.indexOf('async fn prepare_petui_authoring'),lib.indexOf('async fn build_petui_project'));
 assert.match(prepare,/let home = get_home_dir\(\)\?/);assert.match(prepare,/"skillPath": home\.join\("\.agents\/skills\/petui"\)/);
 const launcher=fs.readFileSync(new URL('../skills/petui/scripts/petui.cmd',import.meta.url),'utf8');
 assert.match(launcher,/set "desktop="/);assert.match(launcher,/if not defined USERPROFILE goto missing/);assert.match(launcher,/set "workspace=%USERPROFILE%\\\.claw-pet\\authoring"/);assert(!launcher.includes('%HOME%'));
 assert.match(launcher,/"%desktop%" --petui %\*/);
});
