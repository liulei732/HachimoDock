"""Exercise full directory admission with distinct device applications and an older board."""
import json
from pathlib import Path
import shutil

import pytest
from validate_generated_widget import DEFAULT_CAPABILITIES, validate_widget, valid_private_feed


@pytest.mark.parametrize("component,title,endpoint,connection,key", [
    ("class-checkin", "到达签到", "https://checkin.example.org/visits",
     {"type": "https-submit", "value": "arrived"}, "widgetScopedWrite"),
    ("desk-light", "桌灯控制", "mqtts://desk.example.org",
     {"type": "mqtt-publish", "topic": "room/light", "value": "on"}, "widgetScopedWrite"),
])
def test_scoped_network_apps(tmp_path, component, title, endpoint, connection, key):
    template = Path(__file__).resolve().parents[3] / "builtin-clawpkgs" / "stock-watchlist"
    app = tmp_path / component
    shutil.copytree(template, app)
    manifest = json.loads((app / "component.json").read_text())
    manifest.update(id=component, name=title)
    (app / "component.json").write_text(json.dumps(manifest, ensure_ascii=False))
    runtime = json.loads((app / "runtime/widget.json").read_text())
    runtime["dashboard"].update(title=title, headline=title, note="设备服务账号", footer="设备授权")
    (app / "negative-screen.json").write_text(json.dumps({"dashboard": runtime["dashboard"]}, ensure_ascii=False))
    data = runtime["data"] = {"source": "wifi.feed", "page_var": "list_page", "url": endpoint,
                              "connection": connection}
    (app / "runtime/widget.json").write_text(json.dumps(runtime, ensure_ascii=False))
    assert not validate_widget(app)
    old = dict(DEFAULT_CAPABILITIES)
    del old[key]
    assert validate_widget(app, old)
    assert not valid_private_feed({**data, "connection": {**connection, "token": "forbidden"}})
    assert not valid_private_feed({**data, "response_map": {}})
    if key == "widgetScopedWrite":
        assert not valid_private_feed({**data, "refresh_action": "data.refresh"})
        for bad in ("", "x" * 25, "a b", "\\r", "动态"):
            assert not valid_private_feed({**data, "connection": {**connection, "value": bad}})


def test_retired_github_connection_requires_explicit_new_declaration():
    data = {"source": "wifi.feed", "url": "https://api.github.com/user"}
    assert not valid_private_feed({**data, "connection": {"type": "github-account"}})
    assert valid_private_feed({**data, "connection": {"type": "bearer"}})
