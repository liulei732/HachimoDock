// Builtin previews come directly from build-owned declarations (imported packages use the native manifest).
import contract from '../../skills/petui/contracts/feature-compatibility-v1.json' with {type:'json'};

export function builtinFeatureManifest(id, runtime, app, extension) {
  const fetchRows = app?.engine === 'pc-fetch-v1' && app.content?.some(item =>
    runtime.data?.source === 'fetch.' + item.id && item.map?.rows);
  const python = Object.values(app?.content?.[0]?.sources || {}).find(s => s?.kind === 'python');
  const context = {id,runtime,app,extension:!!extension,fetchRows:!!fetchRows,
    pythonInput:python?.input?.kind,backgroundAlarm:runtime.alarms?.some(a=>a.background === true)};
  const read = path => path.slice(1).split('/').reduce((o,k)=>o?.[k],context);
  const groups = new Set(), features = [];
  let feed;
  for (const {id,group,when,label,paths,requires,pcRequires,grant,capsGroup,deviceBoards,optional} of contract.rules) {
    if (groups.has(group) || (group === 'fallback' && features.length)) continue;
    if (!Object.entries(when).every(([key,want])=>want === '*' ? read(key) != null : (read(key) ?? null) === want)) continue;
    groups.add(group);
    const row = {id,label,paths};
    const caps = {...requires};
    for (const item of contract.capabilities) {
      if (item.group === capsGroup && read(item.when) != null) Object.assign(caps,item.requires);
    }
    if (Object.keys(caps).length) row.requires = caps;
    if (capsGroup === 'feed') feed = caps;
    if (id === 'feed-refresh') row.requires = feed;
    if (deviceBoards) row.deviceBoards = deviceBoards;
    if (pcRequires) row.pcRequires = pcRequires;
    if (grant) row.grant = grant;
    if (optional) row.optional = true;
    features.push(row);
  }
  return {schemaVersion:1,runtime:runtime.engine,features};
}
