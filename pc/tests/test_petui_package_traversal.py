"""Author-side collection limits, deterministic contents and atomic rejection."""
import json
from pathlib import Path
import sys
import tempfile
from unittest import mock

import pytest

SCRIPTS = Path(__file__).resolve().parents[1] / "skills" / "petui" / "scripts"
sys.path.insert(0, str(SCRIPTS))
import pack_hachiapp as pack


def test_oversized_directory_stops_before_enumerating_the_rest(tmp_path):
    class Entry:
        path = str(tmp_path / "empty")
        def is_dir(self, *, follow_symlinks):
            assert not follow_symlinks
            return True

    seen = []
    def many():
        for i in range(100_000):
            seen.append(i)
            yield Entry()

    from contextlib import nullcontext
    with mock.patch.object(pack.os, "scandir", return_value=nullcontext(many())):
        with pytest.raises(ValueError, match="1024"):
            list(pack.package_entries(tmp_path))
    assert len(seen) == 1025


def test_validator_and_isolated_staging_share_the_entry_limit(tmp_path):
    from validate_generated_widget import validate_widget, CLAWPKG_MAX_ENTRIES
    from publish_generated_widget import publish_widget

    source = tmp_path / "oversized"
    source.mkdir()
    for i in range(CLAWPKG_MAX_ENTRIES + 1):
        (source / str(i)).mkdir()
    assert validate_widget(source, {}) == [f"组件源目录超过 {CLAWPKG_MAX_ENTRIES} 个目录项"]
    isolated = tmp_path / "isolated"
    with pytest.raises(RuntimeError) as error:
        publish_widget(source, pet_home=isolated, source_agent="test", capabilities={})
    failed = json.loads(str(error.value))
    assert failed["status"] == "failed"
    assert failed["error"] == f"组件校验失败: 组件源目录超过 {CLAWPKG_MAX_ENTRIES} 个目录项"
    assert not Path(failed["stagingPath"]).exists()
    assert not list((isolated / "components" / "library").iterdir())


def test_file_limit_and_symlink(tmp_path):
    for i in range(121):
        (tmp_path / f"{i}.txt").touch()
    with pytest.raises(ValueError, match="文件数量"):
        pack.package_directory(tmp_path)
    with tempfile.TemporaryDirectory() as folder:
        (Path(folder) / "external").symlink_to(tmp_path, target_is_directory=True)
        with pytest.raises(ValueError, match="不安全"):
            pack.package_directory(Path(folder))


def test_isolated_staging_preserves_named_assets_and_hash(tmp_path):
    from publish_generated_widget import publish_widget
    from validate_generated_widget import load_capabilities
    example = SCRIPTS.parent / "examples" / "local-notebook"
    caps = {**load_capabilities(None), "widgetAppFormV3": "p4-app-form-v3"}
    first = publish_widget(example, pet_home=tmp_path, source_agent="test", capabilities=caps)
    again = publish_widget(example, pet_home=tmp_path, source_agent="test", capabilities=caps)
    assert first["ok"] and again["reusedExistingVersion"] and first["versionHash"] == again["versionHash"]
    assert (Path(first["publishedPath"]) / "runtime" / "widget.json").read_bytes() == (example / "runtime" / "widget.json").read_bytes()


def test_real_example_is_order_independent_and_atomic(tmp_path, monkeypatch):
    example = SCRIPTS.parent / "examples" / "local-notebook"
    files, module = pack.package_directory(example)
    first = tmp_path / "first.hachiapp"
    second = tmp_path / "second.hachiapp"
    monkeypatch.setattr(pack, "native_validate", lambda _: None)
    pack.write_package(first, files.copy(), module)
    pack.write_package(second, dict(reversed(list(files.items()))), module)
    assert first.read_bytes() == second.read_bytes()
    previous = second.read_bytes()
    def refuse(_):
        raise ValueError("invalid staged package")
    monkeypatch.setattr(pack, "native_validate", refuse)
    with pytest.raises(ValueError, match="invalid staged"):
        pack.write_package(second, files.copy(), module)
    assert second.read_bytes() == previous
