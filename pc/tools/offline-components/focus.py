"""Prepare an opt-in update of the existing timer, preserving its device save identity."""
import json
import shutil

from PIL import Image, ImageOps


def prepare(source, package, portrait, version):
    shutil.copytree(source, package, dirs_exist_ok=True)
    def read(path):
        return json.loads((package/path).read_bytes())
    def write(path, data):
        (package/path).write_text(json.dumps(data, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")

    runtime = read("runtime/widget.json")
    # Old firmware rejects unknown saved fields. Both experiment variables are session-local.
    for name in ("pet_feedback", "pet_greeting"):
        runtime["vars"][name] = {"type":"int", "init":0}
        runtime["limits"][name] = [0,4]
    completion = next(t for t in runtime["tick"] if t["while_state"]=="focus")
    completion["then"]["set"]["pet_greeting"] = {"var":"pet_feedback"}
    rest = next(t for t in runtime["tick"] if t["while_state"]=="rest")
    rest["inc"]["pet_greeting"] = -1
    for transition in runtime["transitions"]:
        if transition["from"] in ("rest","rest_paused"):
            transition.setdefault("set",{})["pet_greeting"] = 0

    surface = runtime["surface"]
    # The device has an 8191-byte descriptor limit. Use its built-in text face
    # and retain the actual controls in place of the redundant footer legend.
    surface.pop("fonts")
    surface["interaction"] = "p4-app-interaction-v1"
    surface["images"] = [asset for asset in surface["images"] if asset["id"]=="background"]
    for name in ("type0.png", "type1.png"):
        (package/"assets"/name).unlink()
    surface["elements"] = [node for node in surface["elements"]
                           if node["y"] < 436 or node.get("text")=="全局键退出"]
    buttons = read("buttons.json")
    for on in (False, True):
        action = "tomato.pet_on" if on else "tomato.pet_off"
        runtime["transitions"].append({"from":"*","on":action,
            "set":{"pet_feedback":4 if on else 0,"pet_greeting":0}})
        buttons.append({"action":action,"control":"前方摇杆",
            "event":"knob.rotate_cw" if on else "knob.rotate_ccw",
            "label":"开启宠物反馈" if on else "关闭宠物反馈"})
        surface["elements"].append({"type":"button","x":30,"y":72,"w":215,"h":48,
            "color":63454,"foreground":16970,"radius":12,"font_size":17,
            "action":action,"text":"开启宠物反馈" if on else "关闭宠物反馈",
            "show":{"var":"pet_feedback","min":0 if on else 1,"max":0 if on else 4}})
    # Only a natural focus completion can set this ephemeral value. No sound, queued
    # notification, system lifecycle transition or background action is dispatched.
    visible={"var":"pet_greeting","min":1,"max":4}
    with Image.open(portrait) as im:
        ImageOps.fit(im.convert("RGB"),(96,96)).save(package/"assets/focus-pet.png")
    surface["images"].append({"id":"pet","asset":"assets/focus-pet.png",
                              "frame_width":96,"frame_height":96,"frames":1,"fps":1})
    surface["elements"].extend([
        {"type":"panel","x":400,"y":171,"w":225,"h":158,"color":63454,"radius":16,"state_mask":4,"show":visible},
        {"type":"image","x":464,"y":179,"w":96,"h":96,"image":"pet","color":65535,"state_mask":4,"show":visible},
        {"type":"text","x":411,"y":281,"w":205,"h":36,"font_size":18,"color":16970,
         "align":"center","text":"陪你完成一轮！","state_mask":4,"show":visible}])
    write("runtime/widget.json",runtime)
    write("buttons.json",buttons)
    component=read("component.json")
    component["version"]=version
    component["description"] += "可开启宠物回合反馈，完成后显示四秒，随时可关闭。"
    write("component.json",component)
    share=read("share.json")
    share["summary"]=component["description"]
    write("share.json",share)
