"""Exercise a new private check-in or text notebook on the explicitly named board."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import secrets
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "firmware/tests/fixtures/record-checkin"
sys.path.insert(0, str(ROOT / "pc/skills/petui/scripts"))
from local_records import compile_runtime
from validate_generated_widget import validate_runtime, validate_buttons


def fixture_runtime(version):
    source = json.loads((FIXTURE / "source.json").read_text())
    if version == 2:
        patch = json.loads((FIXTURE / "upgrade.json").read_text())
        source["records"]["version"] = patch.pop("version")
        source["records"]["actions"].update(patch)
        source["surface"]["elements"].append({"type":"button", "x":220, "y":375,
            "w":210, "h":64, "color":2016, "text":"Upgrade records", "action":"upgrade"})
    return compile_runtime(source)


def wire_files(component, version):
    package = FIXTURE / "package"
    data = {path: json.loads((package / path).read_text())
            for path in ("buttons.json", "component.json", "negative-screen.json", "share.json")}
    data["component.json"].update(id=component, version=f"{version}.0.0")
    runtime = fixture_runtime(version)
    # miniapp/event targets the foreground. Mint unique action names too, so
    # even a simultaneous physical focus change cannot trigger another app.
    def action(name):
        return component+"."+name
    for rule in runtime["transitions"]:
        rule["on"] = action(rule["on"])
    for button in data["buttons.json"]:
        button["action"] = action(button["action"])
    for element in runtime["surface"]["elements"]:
        if "action" in element:
            element["action"] = action(element["action"])
    program = runtime["program"]
    program["actions"] = {action(k):v for k,v in program["actions"].items()}
    data["runtime/widget.json"] = runtime
    return {p: json.dumps(data[p], ensure_ascii=False, separators=(",", ":")).encode()
            for p in sorted(data)}


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def inventory(bench, board):
    data = bench.inventory()
    require(data.get("boardDeviceId") == board and data.get("complete") and data.get("ok")
            and not data.get("recoveryBlocked"), "board or inventory mismatch")
    return data


def catalog(data):
    return {item["id"] for item in data["items"]}


def notebook_wire_files(component, version):
    from text_notebook import notebook_files
    keyboards = ["latin", "pinyin"] + (["latin"] if version == 2 else [])
    files = notebook_files(component, f"{version}.0.0", keyboards, action_prefix=component + ".")
    return {path: json.dumps(files[path], ensure_ascii=False, separators=(",", ":")).encode()
            for path in sorted(files)}


def install(bench, component, version, file_builder=wire_files):
    from build_factory_image import fnv1a64_bytes
    files = file_builder(component, version)
    digest = hashlib.sha256()
    for path, contents in files.items():
        digest.update(path.encode() + b'\0' + len(contents).to_bytes(4, "little") + contents)
    base = {"widgetId":component, "transferId":"records-"+secrets.token_hex(8),
            "packageSha256":digest.hexdigest()}
    bench.transfer("widget", "begin", {**base, "transferProtocol":"p4-widget-files-v2",
                   "fileCount":len(files), "totalBytes":sum(map(len,files.values()))})
    sequence = 0
    for i,(path,contents) in enumerate(files.items()):
        for offset in range(0,len(contents),4096):
            chunk = contents[offset:offset+4096]
            bench.transfer("widget","chunk",{**base, "path":path, "fileIndex":i,
                "fileBytes":len(contents), "offset":offset, "sequence":sequence,
                "index":offset//4096, "decodedSize":len(chunk), "checksum":f"{fnv1a64_bytes(chunk):016x}",
                "data":base64.b64encode(chunk).decode()})
            sequence += 1
    bench.transfer("widget","commit",base,timeout=45)


def wait_state(bench, component, match, digest="vmDigest"):
    deadline = time.monotonic()+20
    s = {}
    while time.monotonic() < deadline:
        s = bench.request("miniapp/query",reply="miniapp/state")
        if s.get("widgetId") == component and s.get("observationAvailable") and s.get(digest) and match(s):
            return s
        time.sleep(.15)
    if s.get("widgetId") != component:
        raise RuntimeError("fixture lost foreground before readback")
    fields = ("widgetId", "bootId", digest, "metricValue", "badge", "note")
    raise RuntimeError(f"fixture readback mismatch: { {k:s.get(k) for k in fields} }")


def matches_snapshot(state, expected):
    return all(state.get(key) == expected[key] for key in
               ("vmDigest", "metricValue", "badge", "note"))


def delete_ack(bench, component, preserve, *, timeout_seconds=120):
    """HIL-only wait for a single delete; the caller may select a shorter board timeout."""
    if type(timeout_seconds) is not int or not 1 <= timeout_seconds <= 120:
        raise ValueError("delete ACK timeout must be an integer from 1 to 120 seconds")
    # USB package ACKs use the transfer identity. Admission failures can be
    # protocol ACKs keyed by request identity. Send exactly once.
    request_id = "records-" + secrets.token_hex(8)
    transfer_id = "widget-delete-" + secrets.token_hex(8)
    bench.send("widget/delete", dict(widgetId=component, preserveSave=preserve,
                                     transferId=transfer_id, requestId=request_id))
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        raw = bench.s.readline().replace(b"\0", b"")
        if b"Guru Meditation" in raw or b"Stack protection fault" in raw:
            raise RuntimeError("device panic during removal")
        try:
            message = json.loads(raw[raw.index(b"{"):])
        except ValueError:
            continue
        if not isinstance(message, dict):
            continue
        topic, ack = message.get("topic"), message.get("payload")
        if not isinstance(ack, dict):
            continue
        package_ack = (topic == "widget-install-ack" and ack.get("transferId") == transfer_id
                       and ack.get("phase") == "delete")
        protocol_ack = (topic == "protocol/ack" and ack.get("requestId") == request_id
                        and ack.get("requestTopic") == "widget/delete")
        if package_ack or protocol_ack:
            require(ack.get("ok") is True, f"delete rejected: {message}")
            return message
    raise TimeoutError(f"missing correlated widget/delete ACK after {timeout_seconds}s; outcome unknown")


def hil(options):
    from local_delights_device import Bench
    if not __debug__:
        raise RuntimeError("bench transport requires enabled ACK assertions")
    if options.port is None:
        ports = sorted(Path("/dev").glob("cu.usbmodem*"))
        require(len(ports)==1,"automatic port selection requires exactly one USB modem")
        options.port = str(ports[0])
    bench = Bench(options.port)
    board = options.board
    component = "records-hil-"+secrets.token_hex(8)
    notebook=getattr(options,"notebook",False)
    files_for=notebook_wire_files if notebook else wire_files
    def activate():
        bench.request("widget/activate",{"widgetId":component,"show":True})
        runtime=bench.request("diagnostics/query",reply="diagnostics/status")["runtime"]
        require(runtime["screenPage"]=="app", "activation did not open the app screen")
        require(runtime.get("buildId")==build_id, "running firmware changed during the fixture run")
    def remove(preserve):
        require(catalog(inventory(bench,board))==initial|{component},"unexpected catalog before removal")
        try:
            ack = delete_ack(bench, component, preserve)
        except TimeoutError:
            present = component in catalog(inventory(bench, board))
            raise RuntimeError(f"delete ACK missing; fixture present={present}; no replay") from None
        with options.log.open("a") as f:
            f.write(json.dumps(dict(stage="delete", component=component, preserve=preserve, ack=ack))+'\n')
        require(catalog(inventory(bench,board))==initial,"prior catalog not preserved")
    def observe(stage,state):
        with options.log.open("a") as f:
            f.write(json.dumps(dict(stage=stage,component=component,board=board,
                boot=state["bootId"],vm=state["vmDigest"],day=state.get("metricValue"),
                category=state.get("badge"),key=state.get("note")))+'\n')
    def action(name,day,category,key):
        previous=wait_state(bench,component,lambda s: True)
        bench.action(component+"."+name)
        s=wait_state(bench,component,lambda s: s["vmDigest"]["revision"]==previous["vmDigest"]["revision"]+1
                     and [s.get(k) for k in ("metricValue","badge","note")]==list(map(str,(day,category,key))))
        observe(name,s)
        return s
    def reboot(previous_boot, expected_catalog):
        nonlocal bench
        bench.request("system/reboot",reply="diagnostics/action")
        bench.s.close()
        deadline=time.monotonic()+90
        while time.monotonic()<deadline:
            time.sleep(1)
            try:
                bench=Bench(options.port)
                inv=inventory(bench,board)
                boot=bench.request("miniapp/query",reply="miniapp/state").get("bootId")
            except (OSError,TimeoutError):
                bench.s.close()
                continue
            if boot and boot!=previous_boot:
                break
            bench.s.close()
        else:
            raise RuntimeError("different boot on the same board was not observed")
        require(catalog(inv)==expected_catalog,"catalog changed across reboot")

    def run_checkin():
        install(bench,component,1)
        require(catalog(inventory(bench,board))==initial|{component},"catalog changed")
        activate()
        empty=wait_state(bench,component,lambda s:s["vmDigest"]["revision"]==0)
        observe("first entry",empty)
        action("work",1,1,0)
        bench.action(component+".day.up")
        action("rest",2,2,1)
        old=action("browse",1,1,0)
        time.sleep(45)  # store defers flush at least 30 seconds
        reboot(old["bootId"], initial | {component})
        activate()
        restored=wait_state(bench,component,lambda s:matches_snapshot(s,old))
        observe("cold readback",restored)
        install(bench,component,2)
        activate()
        upgrade_entry=wait_state(bench,component,lambda s:matches_snapshot(s,old))
        observe("same-ID upgrade entry",upgrade_entry)
        action("upgrade",1,11,0)
        bench.action(component+".slot.up")
        action("browse",2,12,1)
        action("erase",0,0,-1)
        action("browse",0,0,-1)
        bench.action(component+".first")
        kept=action("browse",1,11,0)
        remove(True)
        reboot(kept["bootId"], initial)  # an immediate reinstall alone could read the RAM cache
        install(bench,component,2)
        activate()
        restored=wait_state(bench,component,lambda s:matches_snapshot(s,kept))
        observe("preserved reinstall",restored)
        remove(False)
        install(bench,component,2)
        activate()
        pristine=wait_state(bench,component,lambda s:matches_snapshot(s,empty))
        observe("cleared entry",pristine)
        fresh=action("browse",0,0,-1)
        observe("cleared reinstall",fresh)
        remove(False)

    def run_notebook():
        from app_form_v3 import candidates

        def keys(*names):
            for name in names:
                bench.action(component + "." + name)

        def rows(stage, expected, digest=None):
            names=("metricValue","badge","note")
            state=wait_state(bench,component,lambda s:tuple(s.get(k) for k in names[:len(expected)])==expected
                             and (digest is None or s["persistProjectionDigest"]==digest),
                             digest="persistProjectionDigest")
            with options.log.open("a") as f:
                f.write(json.dumps(dict(stage=stage, component=component, board=board, boot=state["bootId"],
                    rows=expected, digest=state["persistProjectionDigest"], viewRevision=state["revision"]),
                    ensure_ascii=False)+'\n')
            return state

        def open_version(version):
            install(bench,component,version,notebook_wire_files)
            require(catalog(inventory(bench,board))==initial|{component},"catalog changed")
            activate()

        open_version(1)
        rows("empty notebook",("",""))
        keys("form.accept","text.append","field.next","text.append","form.accept")
        keys("field.next","form.accept","text.append","form.accept","form.accept")
        old_rows=("ab", candidates("2")[0])
        old=rows("two text notes",old_rows)
        time.sleep(45)
        reboot(old["bootId"],initial|{component})
        activate()
        rows("cold text notes",old_rows,old["persistProjectionDigest"])
        open_version(2)
        rows("added empty row",old_rows+("",))
        keys("field.next","field.next","form.accept","text.append","form.accept")
        kept_rows=old_rows+("a",)
        kept=rows("new text row",kept_rows)
        remove(True)
        reboot(kept["bootId"],initial)
        open_version(2)
        rows("preserved text notes",kept_rows,kept["persistProjectionDigest"])
        remove(False)
        open_version(2)
        rows("cleared text notes",("","",""))
        remove(False)

    try:
        caps=bench.recv("hello",timeout=20)["capabilities"]
        before=inventory(bench,board)
        initial=catalog(before)
        require(component not in initial and len(initial)<before["maxInstalled"],"no free catalog slot")
        for version in (1,2):
            raw=files_for(component,version)["runtime/widget.json"]
            require(len(raw)<8192,"fixture compile exceeds device JSON quota")
            runtime=json.loads(raw)
            errors=[]
            package = ROOT/"pc/skills/petui/examples/local-notebook" if notebook else FIXTURE/"package"
            actions=validate_runtime(runtime,"tool",package,caps,errors)
            buttons=json.loads(files_for(component,version)["buttons.json"])
            validate_buttons(buttons,actions,caps,runtime,errors)
            require(not errors, f"actual board rejects fixture: {errors}")
            if notebook:
                continue
            image=runtime["program"]["image"]
            require(not errors and all(caps.get(k,0)>=v for k,v in (
                ("widgetProgramMaxCells",image["cells"]),("widgetProgramMaxCode",len(image["code"])),
                ("widgetProgramMaxFuel",image["fuel"]),("widgetProgramQuantum",64))),
                "actual board does not admit this program")
        require(caps.get("widgetTransfer")=="p4-widget-files-v2" and
                caps.get("widgetStateObservation")=="p4-miniapp-observation-fnv1a32-v1" and
                caps.get("widgetActivation")=="p4-widget-activation-v1" and
                caps.get("widgetDeletePreserveSave") is True,"missing HIL capability")
        runtime=bench.request("diagnostics/query",reply="diagnostics/status")["runtime"]
        original_page=runtime["screenPage"]
        build_id=runtime.get("buildId")
        # A run never adopts a previous run's identity. An interrupted transfer
        # has an unknown outcome and is never automatically replayed or deleted.
        with options.log.open("x") as f:
            f.write(json.dumps(dict(stage="start",component=component,board=board,buildId=build_id))+'\n')
        if notebook:
            run_notebook()
        else:
            run_checkin()
        if before.get("activeWidgetId"):
            bench.request("widget/activate",{"widgetId":before["activeWidgetId"],"show":False})
        if before.get("selectedWidgetId"):
            bench.request("widget/select_by_id",{"widgetId":before["selectedWidgetId"]})
        bench.request("control/screen-page",{"page":original_page})
        after=inventory(bench,board)
        require(catalog(after)==initial and all(after.get(k)==before.get(k)
                for k in ("activeWidgetId","selectedWidgetId") if before.get(k)),
                "original catalog state not restored")
        with options.log.open("a") as f:
            f.write(json.dumps(dict(stage="complete",component=component,board=board))+'\n')
    finally:
        bench.s.close()


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--notebook",action="store_true",help="exercise native UTF-8 notes and the additive schema upgrade")
    parser.add_argument("--port",help="exclusive P4 port; default is the sole macOS USB modem")
    parser.add_argument("--board",required=True)
    parser.add_argument("--log",required=True,type=Path)
    hil(parser.parse_args())
