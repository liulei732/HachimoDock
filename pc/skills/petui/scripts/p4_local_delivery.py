"""Compile the deliberately small WiFi6 p4-local-v1 device delivery profile."""
import hashlib
import json
import re
import struct


PROFILE = "p4-local-v1"
SOURCE_JSON = ("component.json", "runtime/widget.json", "buttons.json")
SOURCE_JSON_LIMIT = 64 * 1024
FILES = {"component.json", "negative-screen.json", "runtime/widget.json", "buttons.json",
         "share.json", "assets/.keep", "hachiapp.json", "pc/cover.png", "LICENSE.txt",
         "THIRD_PARTY_NOTICES.txt"}
DASHBOARD = {"title", "headline", "footer", "metricValue", "visualStyle", "visualPalette",
             "visualLayout", "visualSprite"}


def compact(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(",", ":"),
                      allow_nan=False).encode("utf-8")


def printable(value, size):
    return isinstance(value, str) and value.isprintable() and 0 < len(value.encode()) <= size


def integer(value, low=-1_000_000_000, high=1_000_000_000):
    return type(value) is int and low <= value <= high


def identifier(value):
    return isinstance(value, str) and re.fullmatch(r"[a-z][a-z0-9_]{0,22}", value)


def dashboard_value(value, variables, states):
    if printable(value, 96):
        return True
    if not isinstance(value, dict):
        return False
    if set(value) == {"var"}:
        return identifier(value["var"]) and value["var"] in variables
    branches = value.get("switch_state")
    return (set(value) == {"switch_state"} and isinstance(branches, dict) and branches
            and all(key in states and printable(label, 96) for key, label in branches.items()))


def button_actions(buttons):
    if not isinstance(buttons, list) or len(buttons) > 2:
        return None
    actions, controls = set(), set()
    for binding in buttons:
        if not isinstance(binding, dict) or set(binding) != {"event", "control", "action", "label"}:
            return None
        control, action = binding["control"], binding["action"]
        if (control not in ("SW1", "SW2") or control in controls
                or binding["event"] != f"button.{control.lower()}.short_press"
                or not identifier(action) or not action.startswith("local_") or action in actions
                or not printable(binding["label"], 30)):
            return None
        actions.add(action)
        controls.add(control)
    return actions


def local_rule(rule, actions, states, variables):
    if not isinstance(rule, dict) or not {"from", "on"} <= set(rule) <= {"from", "on", "to", "set", "inc"}:
        return False
    if (rule["from"] not in ["*", *states] or not identifier(rule["on"]) or rule["on"] not in actions
            or ("to" in rule and rule["to"] not in states)):
        return False
    for op in ("set", "inc"):
        if op in rule:
            writes = rule[op]
            if not isinstance(writes, dict) or not writes or not all(
                    name in variables and integer(number) for name, number in writes.items()):
                return False
    return True


def local_surface(surface, dashboard):
    if (not isinstance(surface, dict) or set(surface) != {"engine", "graphics", "background", "elements"}
            or surface["engine"] != "p4-app-surface-v1" or surface["graphics"] != "p4-app-graphics-v1"
            or not integer(surface["background"], 0, 65535)):
        return False
    elements = surface["elements"]
    if not isinstance(elements, list) or not 1 <= len(elements) <= 8:
        return False
    for element in elements:
        fields = {"type", "x", "y", "w", "h", "color"}
        if not isinstance(element, dict) or not fields <= set(element):
            return False
        if element["type"] == "text":
            if set(element) == fields | {"bind"}:
                if not isinstance(element["bind"], str) or element["bind"] not in dashboard:
                    return False
            elif set(element) != fields | {"text"} or not printable(element["text"], 96):
                return False
        elif element["type"] != "rect" or set(element) != fields:
            return False
        x, y = element["x"], element["y"]
        if not (integer(x, 0, 639) and integer(y, 0, 479) and integer(element["w"], 1, 640-x)
                and integer(element["h"], 1, 480-y) and integer(element["color"], 0, 65535)):
            return False
    return True


def local_runtime(runtime, actions):
    required = {"engine", "schema_version", "states", "initial_state", "vars", "tick", "transitions", "dashboard"}
    if (not isinstance(runtime, dict) or not required <= set(runtime) <= required | {"surface", "persist"}
            or runtime["engine"] != "p4-bounded-runtime-v4" or not integer(runtime["schema_version"], 1, 1)
            or runtime["tick"] != []):
        return False
    states, variables = runtime["states"], runtime["vars"]
    if (not isinstance(states, list) or not 1 <= len(states) <= 6 or not all(map(identifier, states))
            or len(states) != len(set(states)) or runtime["initial_state"] not in states
            or not isinstance(variables, dict) or len(variables) > 8):
        return False
    for name, var in variables.items():
        if (not identifier(name) or not isinstance(var, dict) or set(var) != {"type", "init"}
                or var["type"] != "int" or not integer(var["init"])):
            return False
    rules, dashboard = runtime["transitions"], runtime["dashboard"]
    if (not isinstance(rules, list) or len(rules) > 12
            or not all(local_rule(rule, actions, states, variables) for rule in rules)
            or {rule["on"] for rule in rules} != actions or not isinstance(dashboard, dict)
            or not dashboard or set(dashboard) - DASHBOARD
            or not all(dashboard_value(v, variables, states) for v in dashboard.values())):
        return False
    if "surface" in runtime and not local_surface(runtime["surface"], dashboard):
        return False
    if "persist" in runtime:
        persist = runtime["persist"]
        if ("surface" not in runtime or not isinstance(persist, dict) or set(persist) != {"version", "vars", "states"}
                or not integer(persist["version"], 1, 65535)):
            return False
        saved, restore = persist["vars"], persist["states"]
        return (isinstance(saved, list) and 1 <= len(saved) <= len(variables)
                and all(identifier(v) and v in variables for v in saved) and len(saved) == len(set(saved))
                and isinstance(restore, dict) and set(restore) == set(states)
                and all(v in states for v in restore.values()))
    return True


def compile_payload(files):
    """Return None for desktop-only content. Call after ordinary archive validation."""
    if any(path not in FILES and not re.fullmatch(r"licenses/[A-Za-z0-9_.-]+\.(txt|md)", path)
           for path in files):
        return None
    if any(len(files[path]) > SOURCE_JSON_LIMIT for path in SOURCE_JSON):
        return None
    info = json.loads(files["component.json"])
    if (set(info) - {"id", "name", "version", "kind", "description"} or info["kind"] != "tool"
            or not re.fullmatch(r"[a-z][a-z0-9_-]{0,42}", info["id"])
            or not printable(info["name"], 96) or len(info["version"]) > 32
            or not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", info["version"])):
        return None
    runtime = json.loads(files["runtime/widget.json"])
    buttons = json.loads(files["buttons.json"])
    if not isinstance(runtime, dict):
        return None
    # Match the native prepare_p4_widget_file container defaults for older inputs.
    runtime.setdefault("vars", {})
    if runtime["vars"] == []:
        runtime["vars"] = {}
    runtime.setdefault("transitions", [])
    runtime.setdefault("tick", [])
    actions = button_actions(buttons)
    if actions is None or not local_runtime(runtime, actions):
        return None
    widget = compact(runtime)
    bindings = compact(buttons)
    if len(widget) > 4095 or len(bindings) > 2047:
        return None
    frames = bytearray()
    for name, data in ((b"buttons.json", bindings), (b"runtime/widget.json", widget)):
        frames.extend(name + b"\0" + struct.pack("<I", len(data)) + data)
    payload = bytes(frames)
    return {"payload": payload, "sha256": hashlib.sha256(payload).hexdigest(),
            "fileCount": 2, "downloadBytes": len(payload), "installBytes": len(widget) + len(bindings),
            "privileges": int("persist" in runtime)}
