"""Resolve the local configuration path for desktop code signing."""
import argparse
import json
import os
from pathlib import Path


def _absolute(value, name):
    if not value or not Path(value).is_absolute():
        raise RuntimeError(f'{name} must be a non-empty absolute path')
    return Path(value).resolve()


def workspace_output(workspace, *parts):
    """Reject output traversal and existing links escaping the selected workspace."""
    workspace = Path(workspace).resolve()
    path = workspace.joinpath(*parts).resolve()
    if path == workspace or workspace not in path.parents:
        raise RuntimeError('Build output escapes workspace')
    return path


def resolve_local_paths(source, environ=None):
    env = os.environ if environ is None else environ
    source = Path(source).resolve(strict=True)
    explicit_config = 'HACHIMI_LOCAL_CONFIG' in env
    config = _absolute(env['HACHIMI_LOCAL_CONFIG'], 'HACHIMI_LOCAL_CONFIG') if explicit_config else None
    if 'HACHIMI_WORKSPACE_ROOT' in env:
        workspace = _absolute(env['HACHIMI_WORKSPACE_ROOT'], 'HACHIMI_WORKSPACE_ROOT')
    elif config is not None:
        workspace = config.parent
    else:
        candidates = [source.parent]
        if source.parent.name == 'worktrees':
            candidates.append(source.parent.parent)
        workspace = next((p for p in candidates if (p / 'toolchains.local.json').is_file()), None)
        if workspace is None:
            raise RuntimeError('No workspace/toolchains.local.json; configure HACHIMI_WORKSPACE_ROOT or HACHIMI_LOCAL_CONFIG')
    workspace = workspace.resolve(strict=True)
    if workspace in (Path(workspace.anchor), Path.home().resolve(), source):
        raise RuntimeError('Unsafe workspace root')
    # Accept a direct checkout, the source alias, or the legacy worktrees layout.
    belongs = (source.parent == workspace or
               (source.parent.name == 'worktrees' and source.parent.parent == workspace) or
               (workspace / 'source').resolve() == source)
    if not belongs:
        raise RuntimeError('Workspace root does not match source checkout')
    config = config if config is not None else (workspace / 'toolchains.local.json').resolve()
    if config.parent != workspace or not config.is_file():
        raise RuntimeError('Local config must be an existing file directly in workspace root')
    for name in ('.work', '.cache', 'releases'):
        workspace_output(workspace, name)
    return workspace, config


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    args = parser.parse_args()
    workspace, config = resolve_local_paths(args.source)
    # Paths only: never emit signing identities or other configuration values.
    print(json.dumps({'workspaceRoot': str(workspace), 'configPath': str(config)}))
