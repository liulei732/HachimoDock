"""PetUI host Python package contract. Source is inspected without executing it."""

ENTRY = "pc/plugin.py"
MAX_SOURCE = 16 * 1024 - 1


def declared(application):
    if not isinstance(application, dict) or application.get("engine") not in (
        "pc-app-composition-v1", "pc-app-composition-v2", "pc-app-composition-v3",
    ):
        return False
    content = application.get("content")
    if not isinstance(content, list) or len(content) != 1 or not isinstance(content[0], dict):
        return False
    sources = content[0].get("sources")
    return isinstance(sources, dict) and any(
        isinstance(s, dict) and s.get("kind") == "python" for s in sources.values()
    )


def validate_source(data):
    if len(data) > MAX_SOURCE or b"\0" in data or not data.decode("utf-8").strip():
        raise ValueError("Python 入口为空、包含空字节或超过 16 KiB")
