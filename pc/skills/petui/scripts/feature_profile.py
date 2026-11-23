"""Build display-only public feature metadata from already validated declarations."""
import json
from functools import lru_cache
from pathlib import Path


@lru_cache(maxsize=1)
def contract():
    path = Path(__file__).resolve().parents[1] / 'contracts' / 'feature-compatibility-v1.json'
    return json.loads(path.read_text('utf-8'))


def feature_manifest(component_id, runtime, application=None, extension=False):
    app = application or {}
    items = app.get('content')
    if not isinstance(items, list):
        items = []
    sources = (items[0].get('sources') or {}) if items and isinstance(items[0], dict) else {}
    plugin = next((source for source in sources.values()
                   if isinstance(source, dict) and source.get('kind') == 'python'), {}) if isinstance(sources, dict) else {}
    fetch_rows = app.get('engine') == 'pc-fetch-v1' and any(
        isinstance(item, dict) and isinstance(item.get('id'), str)
        and runtime.get('data', {}).get('source') == 'fetch.' + item['id']
        and isinstance(item.get('map', {}).get('rows'), dict) for item in items)
    context = dict(id=component_id, runtime=runtime, app=app, extension=extension,
                   pythonInput=plugin.get('input', {}).get('kind'), fetchRows=fetch_rows,
                   backgroundAlarm=any(a.get('background') is True for a in runtime.get('alarms', [])))

    def read(path):
        value = context
        for key in path[1:].split('/'):
            if isinstance(value, list) and key.isdecimal() and int(key) < len(value):
                value = value[int(key)]
            else:
                value = value.get(key) if isinstance(value, dict) else None
        return value

    groups, features = set(), []
    feed = {}
    for rule in contract()['rules']:
        group = rule['group']
        if group in groups or (group == 'fallback' and features):
            continue
        if not all((read(key) is not None if want == '*' else read(key) == want)
                   for key, want in rule['when'].items()):
            continue
        groups.add(group)
        row = {key: rule[key] for key in ('id', 'label', 'paths')}
        required = dict(rule.get('requires', {}))
        for item in contract()['capabilities']:
            if item['group'] == rule.get('capsGroup') and read(item['when']) is not None:
                required.update(item['requires'])
        if rule.get('capsGroup') == 'feed':
            feed = required
        if rule['id'] == 'feed-refresh':
            required = feed
        if required:
            row['requires'] = required
        if 'deviceBoards' in rule:
            row['deviceBoards'] = rule['deviceBoards']
        if 'pcRequires' in rule:
            row['pcRequires'] = rule['pcRequires']
        if rule.get('optional'):
            row['optional'] = True
        if rule.get('grant'):
            row['grant'] = rule['grant']
        features.append(row)
    return {'schemaVersion': 1, 'runtime': runtime.get('engine'), 'features': features}
