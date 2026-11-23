"""Generate a bounded device-local text notebook from the native PetUI form contract."""
import argparse
import json
from pathlib import Path


FORM_CONTRACT = Path(__file__).resolve().parents[1] / "contracts/app-form-v3.json"
KEYBOARDS = ("latin", "pinyin", "numeric")


def notebook_files(component, version, keyboards, max_bytes=40, action_prefix=""):
    """Each row's stable variable name is its key in the component-private JSON save."""
    if not 1 <= len(keyboards) <= 4 or any(mode not in KEYBOARDS for mode in keyboards):
        raise ValueError("choose one to four native row keyboards")
    if type(max_bytes) is not int or not 1 <= max_bytes <= 63:
        raise ValueError("row byte limit must be between 1 and 63")
    from validate_generated_widget import ID_PATTERN
    if not ID_PATTERN.fullmatch(component) or not version.strip():
        raise ValueError("invalid component ID or version")
    contract = json.loads(FORM_CONTRACT.read_text(encoding="utf-8"))
    form = contract["runtime"]["form"]
    buttons = contract["buttons"]
    for button in buttons:
        button["action"] = action_prefix + button["action"]
    form["actions"] = {op: action_prefix + action for op, action in form["actions"].items()}
    form["fields"] = []
    variables = {name: {"type": "int", "init": 0} for name in ("focus", "editing", "key", "cursor")}
    variables.update({name: {"type": "string", "init": ""} for name in ("label", "buffer", "cands")})
    rows = []
    elements = []

    def text(x, y, width, **content):
        elements.append(dict(type="text", x=x, y=y, w=width, h=32, color=65535, **content))

    for index, mode in enumerate(keyboards):
        row = f"note{index + 1}"
        rows.append(row)
        draft = row + "Draft"
        variables[row] = {"type": "string", "init": ""}
        variables[draft] = {"type": "string", "init": ""}
        form["fields"].append(dict(id=row, var=row, draft_var=draft, kind="text",
                                   max_bytes=max_bytes, keyboard=mode))
        text(16, 22 + index * 46, 120, text=f"{index}  {mode}")
        text(142, 22 + index * 46, 478, field_bind=row)

    text(16, 222, 90, text="Row")
    text(110, 222, 42, var_bind="focus")
    text(184, 222, 106, text="Editing")
    text(298, 222, 40, var_bind="editing")
    text(16, 262, 294, var_bind="label")
    text(316, 262, 302, var_bind="cands")
    text(16, 316, 608, text="Stick up/down: row or letter. Turn: key group.")
    text(16, 361, 608, text="Press knob: append. Hold knob: erase.")
    text(16, 406, 608, text="SW1: edit, accept or save. SW2: cancel.")
    dashboard = dict(title="Pocket notebook", metricLabel="Note 1", metricValue={"var": rows[0]},
                     visualStyle="clean", visualPalette="mint", visualLayout="tool", visualSprite="star")
    for slot, row in zip(("badge", "note"), rows[1:]):
        dashboard[slot] = {"var": row}
    # parse_save budgets six JSON escape bytes per input byte plus envelope and keys.
    budget = 128 + sum(len(row) + 6 + 6 * max_bytes for row in rows)
    runtime = dict(schema_version=1, engine="p4-bounded-runtime-v4",
                   quota="p4-quota-v2" if budget <= 1023 else "p4-quota-v3",
                   states=["ready"], initial_state="ready", pages=[{"id": "edit"}], initial_page="edit",
                   vars=variables, form=form, input={"engine": "p4-input-v1"},
                   transitions=[{"from": "*", "on": action} for action in form["actions"].values()],
                   tick=[], dashboard=dashboard, persist=dict(version=1, vars=rows, states={"ready": "ready"}),
                   surface=dict(engine="p4-app-surface-v1", graphics="p4-app-graphics-v1",
                                bindings="p4-variable-bindings-v1", background=0, elements=elements))
    return {
        "component.json": dict(id=component, name="Pocket notebook", version=version, kind="tool",
                               description="Edit short private notes with the on-device keyboard."),
        "buttons.json": buttons, "runtime/widget.json": runtime,
        "negative-screen.json": {"dashboard": dict(title="Pocket notebook", metricLabel="Note 1",
                                                   metricValue="", visualStyle="clean",
                                                   visualPalette="mint", visualLayout="tool", visualSprite="star")},
        "share.json": {"title": "Pocket notebook"},
    }


def write_package(directory, files):
    directory.mkdir()  # create a new package without replacing existing author files
    (directory / "runtime").mkdir()
    (directory / "assets").mkdir()
    (directory / "assets/.keep").touch()
    for path, data in files.items():
        (directory / path).write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="new component package directory")
    parser.add_argument("--id", required=True)
    parser.add_argument("--version", default="1.0.0")
    parser.add_argument("--keyboards", nargs="+", choices=KEYBOARDS, default=["latin", "pinyin"])
    parser.add_argument("--max-bytes", type=int, default=40)
    args = parser.parse_args()
    write_package(args.directory, notebook_files(args.id, args.version, args.keyboards, args.max_bytes))
