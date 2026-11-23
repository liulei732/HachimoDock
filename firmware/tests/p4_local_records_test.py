"""Run creator-generated actions on the production integer core and parser."""
from copy import deepcopy
import json
from pathlib import Path
import subprocess
import sys

import pytest
from p4_component_program_test import native

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/"firmware/tools"))
from record_checkin_hil import FIXTURE, fixture_runtime, wire_files
from local_records import compile_runtime
from validate_generated_widget import DEFAULT_CAPABILITIES, validate_widget, validate_runtime

CAPS = {**DEFAULT_CAPABILITIES, "widgetProgram":"p4-component-program-v2"}


def test_installable_fixture_and_native_parser(native):
    assert not validate_widget(FIXTURE/"package", CAPS)
    assert json.loads((FIXTURE/"package/runtime/widget.json").read_text()) == fixture_runtime(1)
    for version in (1,2):
        r=fixture_runtime(version)
        assert native(r)["enabled"] and not native(r,enabled=0)["ok"]
        errors=[]
        validate_runtime(r,"tool",FIXTURE/"package",CAPS,errors)
        assert not errors
        files=wire_files("records-hil-123",version)
        assert json.loads(files["component.json"])["id"]=="records-hil-123"
        wire=json.loads(files["runtime/widget.json"])
        assert native(wire)["enabled"] and wire["program"]["image"]==r["program"]["image"]
        assert all(action.startswith("records-hil-123.") for action in wire["program"]["actions"])
        assert len(wire_files("records-hil-"+"0"*16,version)["runtime/widget.json"])<8192


def run_vm(runtimes, javascript):
    source = """
import assert from 'node:assert/strict';
import {createProgramPreview,encodeProgram} from './pc/skills/petui/scripts/program-core.mjs';
let input='';for await (const buf of process.stdin) input+=buf;
const programs=JSON.parse(input);
function open(index,seed=null) {
 const p=programs[index], vm=createProgramPreview(encodeProgram(p.image),{seed,quantum:7});
 let sequence=0;
 return {snapshot:()=>vm.snapshot(), run(name,vars={},expected='committed') {
   const words=p.actions[name].map(w=>typeof w==='number'?w:vars[w.var]);
   assert.equal(vm.enqueue(++sequence,words),'ok');
   while(vm.pump()==='yielded'){}
   const report=vm.takeReport();
   assert.equal(report.result,expected);
   return vm.snapshot().outputs.slice(0,4);
 }};
}
"""
    subprocess.run(["node","--input-type=module","-e",source+javascript],
                   input=json.dumps([r["program"] for r in runtimes]),text=True,cwd=ROOT,check=True)


def test_checkin_rows_fill_enumerate_delete_restart_migrate_and_reject_downgrade():
    run_vm([fixture_runtime(1),fixture_runtime(2)],"""
let a=open(0);
assert.deepEqual(a.run('work',{day:34}),[1,0,34,1]);
assert.deepEqual(a.run('rest',{day:37}),[1,1,37,2]);
assert.deepEqual(a.run('work',{day:42}),[1,2,42,1]);
assert.deepEqual(a.run('rest',{day:43}),[1,3,43,2]);
const full=a.snapshot();
assert.deepEqual(a.run('work',{day:50}),[-2,-1,0,0]);
assert.deepEqual(a.snapshot().values,full.values);
assert.deepEqual(a.run('erase',{cursor:1}),[0,-1,0,0]);
assert.deepEqual(a.run('browse',{cursor:1}),[1,2,42,1]);
a=open(0,a.snapshot());
assert.deepEqual(a.run('browse',{cursor:0}),[1,0,34,1]);
let b=open(1,a.snapshot()),before=b.snapshot().values;
assert.deepEqual(b.run('rest',{day:51}),[-3,-1,0,0]);
assert.deepEqual(b.snapshot().values,before);
assert.deepEqual(b.run('upgrade'),[1,0,34,11]);
assert.deepEqual(b.run('upgrade'),[-3,-1,0,0]);
assert.deepEqual(b.run('rest',{day:51}),[1,1,51,12]);
assert.deepEqual(b.run('browse',{cursor:3}),[1,3,43,12]);
assert.deepEqual(b.run('browse',{cursor:4}),[0,-1,0,0]);
assert.deepEqual(b.snapshot().values.slice(-3),[1,2,0]);
a=open(0,b.snapshot());before=a.snapshot().values;
assert.deepEqual(a.run('work',{day:1}),[-3,-1,0,0]);
assert.deepEqual(a.snapshot().values,before);
""")


def test_atomic_failure_late_in_batch_and_overflow_halfway_through_migration():
    source=json.loads((FIXTURE/"source.json").read_text())
    source["records"]["actions"]={"batch":{"inputs":["cursor"],"batch":[
        ["put","cache",0,99,1],["put","user","cursor",10,3]]}}
    r=compile_runtime(source)
    run_vm([r,fixture_runtime(2)],"""
const a=open(0);
const initial=a.snapshot().values;
assert.deepEqual(a.run('batch',{cursor:4}),[-1,-1,0,0]);
assert.deepEqual(a.snapshot().values,initial);
assert.deepEqual(a.run('batch',{cursor:1}),[1,1,10,3]);
const s=a.snapshot();s.values.splice(1,3,1,23,1);s.values[6]=2147483647;
const b=open(1,s);
b.run('upgrade',{},'overflow');
assert.deepEqual(b.snapshot(),s);
""")


@pytest.mark.parametrize("change",[
    {"user_slots":127,"words":6},{"cache_slots":-1},{"words":True},
    {"actions":{"bad":{"from_version":1,"batch":[["get","user",0]]}}},
    {"actions":{"bad":{"batch":[["put","cache",1,1,2]]}}},
    {"actions":{"bad":{"batch":[["put","user",0,0,0],["append","user",1,2]]}}},
])
def test_quota_and_authoring_rejections(change):
    source=json.loads((FIXTURE/"source.json").read_text())
    source["records"].update(deepcopy(change))
    with pytest.raises(ValueError):
        compile_runtime(source)


def test_compiled_package_commits_to_real_ab_store_then_cold_migrates(tmp_path):
    from petui_app_store_test import _run_private_save_generations
    driver=""
    for version in (1,2):
        p=fixture_runtime(version)["program"]
        im=p["image"]
        rows=",\n".join("{PET_OP_%s,%d,%d,%d,%d}" % tuple(row) for row in im["code"])
        driver+=f"""
static const pet_program_image_t checkin_v{version}={{
 .schema={im['schema']},.cells={im['cells']},.fuel={im['fuel']},.count={len(im['code'])},
 .valid=true,.code={{{rows}}}
}};
"""
        for op,words in p["actions"].items():
            driver+=f"enum {{ v{version}_{op}={words[0]} }};\n"
    driver+=r"""
static pet_program_runtime_t checkin;
static uint64_t generation=1;
static void record_event(unsigned seq, int op, int arg){
 pet_program_event_t e={.count=2,.words={op,arg}};
 assert(pet_program_runtime_enqueue(&checkin,generation,seq,&e)==PET_RUNTIME_OK);
 pet_program_report_t report={0};
 for(int step=0;step<128;step++){
   pet_program_runtime_pump(&checkin,generation);
   if(pet_program_runtime_take_report(&checkin,generation,&report)==PET_RUNTIME_OK){
     assert(report.result==PET_PROGRAM_COMMITTED);return;
   }
 }
 assert(!"compiled action did not complete within the fuel budget");
}
int main(void){
 const char *id="record-checkin";pet_program_snapshot_t snapshot={0},seed={0};
 pet_fs_activity_enable_observation();assert(pet_app_store_init());
 assert(pet_program_persistence_load(id,&checkin_v1,&seed)==PET_PROGRAM_SAVE_EMPTY);
 pet_program_runtime_init(&checkin);
 assert(pet_program_runtime_start(&checkin,&checkin_v1,generation,program_current,&generation,7,NULL)==PET_RUNTIME_OK);
 record_event(1,v1_work,34);record_event(2,v1_rest,37);
 assert(pet_program_runtime_snapshot(&checkin,generation,&snapshot)==PET_RUNTIME_OK);
 assert(snapshot.values[0]==1&&snapshot.values[2]==34&&snapshot.values[3]==1&&snapshot.values[5]==37&&snapshot.values[6]==2);
 assert(pet_program_persistence_enqueue(id,&snapshot));
 assert(pet_program_persistence_checkpoint(id,&snapshot)==PET_APP_CHECKPOINT_PENDING);
 clock_us+=31000000;for(int k=0;k<40;k++)flush_one();
 assert(pet_program_persistence_checkpoint(id,&snapshot)==PET_APP_CHECKPOINT_DURABLE);
 assert(pet_program_runtime_cancel(&checkin,generation)==PET_RUNTIME_OK);
 memset(saves,0,sizeof(saves));pet_program_runtime_init(&checkin);generation++;
 assert(pet_program_persistence_load(id,&checkin_v2,&seed)==PET_PROGRAM_SAVE_VALUE);
 assert(!memcmp(&snapshot,&seed,sizeof(seed)));
 assert(pet_program_runtime_start(&checkin,&checkin_v2,generation,program_current,&generation,7,&seed)==PET_RUNTIME_OK);
 record_event(1,v2_upgrade,0);
 assert(pet_program_runtime_snapshot(&checkin,generation,&snapshot)==PET_RUNTIME_OK);
 assert(snapshot.values[0]==2&&snapshot.values[2]==34&&snapshot.values[3]==11&&snapshot.values[5]==37&&snapshot.values[6]==12);
 assert(pet_program_persistence_enqueue(id,&snapshot));
 assert(pet_program_persistence_checkpoint(id,&snapshot)==PET_APP_CHECKPOINT_PENDING);
 save_io_t removal={0};assert(save_io_begin(&removal,NULL));
 assert(pet_app_store_preserve_with_lease(id,removal.lease));save_io_end(&removal);
 assert(pet_program_persistence_checkpoint(id,&snapshot)==PET_APP_CHECKPOINT_DURABLE);
 memset(saves,0,sizeof(saves));assert(pet_program_persistence_load(id,&checkin_v2,&seed)==PET_PROGRAM_SAVE_VALUE);
 assert(!memcmp(&snapshot,&seed,sizeof(seed)));
 // A newer RAM update during fsync is not covered by the first write's ACK.
 char out[PET_APP_SAVE_BYTES];assert(pet_app_store_load("live",out)==PET_APP_STORE_EMPTY);
 assert(pet_app_store_update("live","first"));
 assert(save_io_begin(&removal,NULL));sync_interleave=1;
 assert(!pet_app_store_preserve_with_lease("live",removal.lease));save_io_end(&removal);
 assert(saves[lookup_slot(0,"live")].dirty);
 pet_resources_device_realtime_end(&realtime);
 assert(save_io_begin(&removal,NULL));
 assert(pet_app_store_preserve_with_lease("live",removal.lease));save_io_end(&removal);
 memset(saves,0,sizeof(saves));assert(pet_app_store_load("live",out)==PET_APP_STORE_VALUE);
 assert(!strcmp(out,"{\"count\":101}"));return 0;
}
"""
    _run_private_save_generations(tmp_path,False,record_driver=driver)


def test_board_transfer_matches_manifest_and_uses_only_its_run_specific_actions():
    import base64
    import hashlib
    from record_checkin_hil import install
    from build_factory_image import fnv1a64_bytes

    class Capture:
        def __init__(self):
            self.calls = []

        def transfer(self, kind, phase, payload, timeout=20):
            assert kind=="widget"
            self.calls.append((phase,payload))

    target="records-hil-"+"f"*16
    for version in (1,2):
        bench=Capture()
        install(bench,target,version)
        phase,begin=bench.calls[0]
        last,commit=bench.calls[-1]
        assert phase=="begin" and last=="commit" and begin["widgetId"]==commit["widgetId"]==target
        files=wire_files(target,version)
        assert begin["fileCount"]==len(files) and begin["totalBytes"]==sum(map(len,files.values()))
        reconstructed={}
        for sequence,(phase,chunk) in enumerate(bench.calls[1:-1]):
            assert phase=="chunk" and chunk["sequence"]==sequence
            path=list(files)[chunk["fileIndex"]]
            body=reconstructed.setdefault(path,bytearray())
            assert chunk["path"]==path and chunk["fileBytes"]==len(files[path])
            assert chunk["offset"]==len(body) and chunk["index"]==len(body)//4096
            raw=base64.b64decode(chunk["data"],validate=True)
            assert 0<len(raw)<=4096 and len(raw)==chunk["decodedSize"] and f"{fnv1a64_bytes(raw):016x}"==chunk["checksum"]
            body.extend(raw)
        assert reconstructed==files
        packet=b"".join(p.encode()+b"\0"+len(v).to_bytes(4,"little")+v for p,v in reconstructed.items())
        assert begin["packageSha256"]==commit["packageSha256"]==hashlib.sha256(packet).hexdigest()
        runtime=json.loads(files["runtime/widget.json"])
        assert all(rule["on"].startswith(target+".") for rule in runtime["transitions"])
        assert all(button["action"].startswith(target+".") for button in json.loads(files["buttons.json"]))


@pytest.mark.parametrize("notebook",[False, True])
def test_hil_script_uses_fresh_identity_reboots_verifies_rows_and_restores_catalog(tmp_path,monkeypatch,notebook):
    """Exercise the actual HIL conversation offline, including refusal and both deletion policies."""
    from types import SimpleNamespace
    import record_checkin_hil as hil
    import app_form_v3 as form
    component_ids={"existing-user-app"}
    saves={}
    disk={}
    context={"active":"existing-user-app","selected":"existing-user-app","boot":1,"page":"main"}
    admissions=[]
    class Bench:
        def __init__(self,port):
            self.s=SimpleNamespace(close=lambda:None,readline=lambda:self.frames.pop(0))

        def recv(self,topic,timeout):
            assert topic=="hello"
            return {"capabilities":{**CAPS,"widgetAppFormV3":"p4-app-form-v3","widgetTransfer":"p4-widget-files-v2",
                "widgetStateObservation":"p4-miniapp-observation-fnv1a32-v1",
                "widgetActivation":"p4-widget-activation-v1","widgetDeletePreserveSave":True,
                "widgetProgramMaxCells":256,"widgetProgramMaxCode":1024,
                "widgetProgramMaxFuel":4096,"widgetProgramQuantum":64}}

        def inventory(self):
            return dict(boardDeviceId="board",complete=True,ok=True,items=[{"id":k} for k in component_ids],
                        maxInstalled=2,activeWidgetId=context["active"],selectedWidgetId=context["selected"])

        def request(self,topic,p=None,reply=None):
            if topic=="diagnostics/query":
                return {"runtime":{"screenPage":context["page"], "buildId":"candidate"}}
            if topic=="miniapp/query":
                current=saves.get(context["active"],{})
                if notebook:
                    return dict(widgetId=context["active"],bootId=str(context["boot"]),observationAvailable=True,
                                revision=1,persistProjectionDigest=json.dumps(current,sort_keys=True),
                                metricValue=current.get("note1",""),badge=current.get("note2",""),note=current.get("note3",""))
                outs=current.get("outputs",[0]*8)
                return dict(widgetId=context["active"],bootId=str(context["boot"]),observationAvailable=True,
                            vmDigest=dict(hash=str((current.get("values",[]),outs)),revision=current.get("revision",0)),
                            metricValue=str(outs[2]),badge=str(outs[3]),note=str(outs[1]))
            if topic=="system/reboot":
                context["boot"]+=1
                if context["boot"]==2:  # the first reboot follows the deferred flush window
                    disk.update(deepcopy(saves))
                saves.clear()
                saves.update(deepcopy(disk))
            elif topic=="widget/activate":
                assert p["widgetId"] in component_ids
                context.update(active=p["widgetId"],day=1,cursor=0)
                if notebook and p["show"]:
                    raw=hil.notebook_wire_files(p["widgetId"],context["version"])
                    runtime=json.loads(raw["runtime/widget.json"])
                    values={k:v["init"] for k,v in runtime["vars"].items()}
                    values.update(saves.get(p["widgetId"],{}))
                    editor={"vars":values,"page":runtime["form"]["page"]}
                    form.initialize(runtime,editor)
                    context.update(editor=editor,form=runtime)
                if p["show"]:context["page"]="app"
            elif topic=="widget/select_by_id":
                assert p["widgetId"] in component_ids
                context["selected"]=p["widgetId"]
            elif topic=="control/screen-page":
                context["page"]=p["page"]
            else:
                raise AssertionError(topic)

        def transfer(self,kind,phase,p,timeout=20):
            component=p["widgetId"]
            assert component.startswith("records-hil-")
            if phase=="commit":
                component_ids.add(component)
                admissions.append(component)
                context.update(version=1 if len(admissions)==1 else 2,active=component)
            else:
                assert phase in ("begin", "chunk")

        def send(self, topic, p):
            assert topic=="widget/delete" and p["requestId"] and p["transferId"]
            component=p["widgetId"]
            assert component.startswith("records-hil-")
            component_ids.remove(component)
            if p["preserveSave"]:
                disk[component]=deepcopy(saves[component])
            else:
                disk.pop(component,None)
            saves.pop(component,None)
            ack=dict(phase="delete",transferId=p["transferId"],ok=True)
            self.frames=[json.dumps({"topic":"widget-install-ack","payload":ack}).encode()]

        def action(self,name):
            component=context["active"]
            assert name.startswith(component+".") and component!="existing-user-app"
            if notebook:
                runtime,editor=context["form"],context["editor"]
                assert form.apply(runtime,editor,form.operation(runtime,name))
                saves[component]={k:editor["vars"][k] for k in runtime["persist"]["vars"]}
                return
            op=name[len(component)+1:]
            if op=="day.up":context["day"]+=1
            elif op=="slot.up":context["cursor"]+=1
            elif op=="first":context["cursor"]=0
            else:
                program=fixture_runtime(context["version"])["program"]
                words=[context[w["var"]] if isinstance(w,dict) else w for w in program["actions"][op]]
                js="""
import {createProgramPreview,encodeProgram} from './pc/skills/petui/scripts/program-core.mjs';
let body='';for await (const chunk of process.stdin) body+=chunk;
const [image,seed,words]=JSON.parse(body),vm=createProgramPreview(encodeProgram(image),{seed});
vm.enqueue(1,words);while(vm.pump()==='yielded'){}
if(vm.takeReport().result!=='committed')throw Error('fixture action refused');
console.log(JSON.stringify(vm.snapshot()));
"""
                run=subprocess.run(["node","--input-type=module","-e",js],cwd=ROOT,text=True,
                                   input=json.dumps([program["image"],saves.get(component),words]),
                                   capture_output=True,check=True)
                saves[component]=json.loads(run.stdout)

    monkeypatch.setitem(sys.modules,"local_delights_device",SimpleNamespace(Bench=Bench))
    monkeypatch.setattr(hil,"time",SimpleNamespace(sleep=lambda n:None,monotonic=__import__("time").monotonic))
    options=SimpleNamespace(port="synthetic",board="wrong-board",notebook=notebook,log=tmp_path/"hil.jsonl")
    with pytest.raises(RuntimeError,match="board"):
        hil.hil(options)
    assert not options.log.exists() and not admissions
    options.board="board"
    hil.hil(options)
    assert context["active"]==context["selected"]=="existing-user-app"
    assert component_ids=={"existing-user-app"} and not saves and context["boot"]==3 and not disk
    assert len(admissions)==4 and len(set(admissions))==1
    stages=[json.loads(line)["stage"] for line in options.log.read_text().splitlines()]
    expected=({"cold text notes","added empty row","preserved text notes","cleared text notes"} if notebook else
              {"cold readback","preserved reinstall","cleared reinstall"})
    assert stages[-1]=="complete" and expected<=set(stages)


def test_hil_rejects_stale_projection_or_revision_even_with_matching_hash():
    from record_checkin_hil import matches_snapshot
    before = dict(vmDigest=dict(schema=7, cells=16, revision=3, hash="913a7634"),
                  metricValue="1", badge="1", note="0")
    assert matches_snapshot(dict(before, bootId="fresh-boot"), before)
    for key in ("metricValue", "badge", "note"):
        assert not matches_snapshot(dict(before, **{key:"stale"}), before)
    assert not matches_snapshot(dict(before, vmDigest=dict(before["vmDigest"],revision=2)), before)


@pytest.mark.parametrize("refuse", [True, False])
def test_delete_ack_correlates_both_ids_and_never_replays(refuse):
    from record_checkin_hil import delete_ack
    from types import SimpleNamespace

    class Link:
        def send(self, topic, request):
            assert topic == "widget/delete" and request["preserveSave"] is True
            assert not hasattr(self, "s")
            transfer = request["transferId"]
            ack = dict(transferId=transfer, phase="delete", ok=True)
            responses = [
                ("widget-install-ack", dict(ack, phase="commit")),
                ("widget-install-ack", dict(ack, transferId="other")),
                ("protocol/ack", dict(requestTopic=topic, requestId="other", ok=False)),
                ("protocol/ack", dict(requestTopic=topic, requestId=request["requestId"], ok=False))
                if refuse else ("widget-install-ack", ack),
            ]
            frames = iter(json.dumps(dict(topic=t,payload=p)).encode() for t,p in responses)
            self.s = SimpleNamespace(readline=lambda:next(frames))

    if refuse:
        with pytest.raises(RuntimeError,match="delete rejected"):
            delete_ack(Link(), "fixture", True)
    else:
        assert delete_ack(Link(), "fixture", True)["payload"]["phase"] == "delete"


@pytest.mark.parametrize("timeout,ack_at,ok,elapsed", [
    (None, 91, True, 91), (60, 91, False, 60),
    (None, 121, False, 120), (1, None, False, 1),
])
def test_delete_wait_is_bounded_without_replay(monkeypatch, timeout, ack_at, ok, elapsed):
    from types import SimpleNamespace
    import record_checkin_hil as hil

    class Link:
        seconds = 0

        def send(self, topic, request):
            assert topic == "widget/delete" and not hasattr(self, "request")
            self.request = request

        def readline(self):
            self.seconds += 1
            transfer = self.request["transferId"] if self.seconds == ack_at else "unrelated"
            return json.dumps({"topic": "widget-install-ack", "payload": {
                "transferId": transfer, "phase": "delete", "ok": True}}).encode()

    link = Link()
    link.s = link
    monkeypatch.setattr(hil, "time", SimpleNamespace(monotonic=lambda: link.seconds))
    options = {} if timeout is None else {"timeout_seconds": timeout}
    if ok:
        ack = hil.delete_ack(link, "fixture", False, **options)
        assert ack["payload"]["transferId"] == link.request["transferId"]
    else:
        with pytest.raises(TimeoutError, match="outcome unknown"):
            hil.delete_ack(link, "fixture", False, **options)
    assert link.seconds == elapsed


@pytest.mark.parametrize("invalid", [0, -1, 121, True, float("inf")])
def test_invalid_delete_timeout_rejected_before_send(invalid):
    from record_checkin_hil import delete_ack
    with pytest.raises(ValueError, match="1 to 120"):
        delete_ack(None, "fixture", False, timeout_seconds=invalid)
