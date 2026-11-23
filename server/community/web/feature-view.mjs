// Shared with the native desktop UI. Reports capabilities, never a grant, connection test or install decision.
const producers = {device:'设备离线', 'device-wifi':'WiFi6 设备联网', pc:'PC', 'pc-usb':'PC 经 USB', 'paired-pc-wifi':'PC 经配对 Wi-Fi', unknown:'来源待核对'};
const statuses = {unsupported:'本板不支持', offline:'设备网络已断开', unconfirmed:'板卡能力待核对',
  'network-unconfirmed':'网络状态待核对', 'capabilities-match':'声明能力匹配', 'pc-required':'需要 PC', unknown:'待核对'};
const own = (o,k)=>Object.prototype.hasOwnProperty.call(o||{},k);
export function featureView(manifest, diagnostic) {
  const valid = manifest?.schemaVersion === 1 && Array.isArray(manifest.features) && manifest.features.length &&
    manifest.features.every(r=>r && typeof r.label === 'string' && Array.isArray(r.paths) && r.paths.length && r.paths.every(p=>own(producers,p)));
  if (!valid) return {summary:'功能来源待核对',modes:['unknown'],rows:[]};
  const target = diagnostic?.ok === false || diagnostic?.connected === false ? null : diagnostic;
  const caps = target?.capabilities ?? target?.usbCapabilities ?? target;
  const board = caps?.firmwareUpdate;
  const model = [board?.hardwareBoardModel,board?.boardModel].find(m=>m === 'wifi6' || m === 'pico');
  const pico = model === 'pico';
  const versions = Array.isArray(caps?.widgetRuntimes) ? caps.widgetRuntimes : null;
  const runtime = caps?.widgetRuntime || versions ? caps?.widgetRuntime === manifest.runtime || !!versions?.includes(manifest.runtime) : null;
  const modes = new Set();
  const rows = manifest.features.map(row=>{
    const paths = {};
    function check(k,v) {
      if (k === 'widgetProgram' && caps?.widgetPrograms?.includes(v)) return '已匹配';
      return !own(caps,k) ? '未上报' : caps[k] === v ? '已匹配' : '不匹配';
    }
    for (const path of row.paths) {
      const device = path === 'device' || path === 'device-wifi';
      const wireless = path === 'device-wifi' || path === 'paired-pc-wifi';
      const checks = Object.entries((device ? row.requires : row.pcRequires)||{});
      const needsRuntime = device || checks.length > 0;
      let status;
      if (path === 'unknown') status = 'unknown';
      else if ((device && model && Array.isArray(row.deviceBoards) && !row.deviceBoards.includes(model)) || (wireless && pico) || (needsRuntime && runtime === false) || checks.some(([k,v])=>check(k,v) === '不匹配')) status = 'unsupported';
      else if (wireless && target?.wifiConnected === false) status = 'offline';
      else if ((needsRuntime && runtime == null) || checks.some(([k,v])=>check(k,v) === '未上报')) status = 'unconfirmed';
      else if (!device) status = 'pc-required';
      else if (wireless && typeof target?.wifiConnected !== 'boolean') status = 'network-unconfirmed';
      else status = 'capabilities-match';
      paths[path] = status;
      if (status !== 'unsupported' && !row.optional) modes.add(path === 'device' ? 'offline' : path === 'device-wifi' ? 'network' : path === 'unknown' ? 'unknown' : 'pc');
    }
    const lines = Object.entries(paths).map(([p,s])=>producers[p] + (s === 'pc-required' || (p === 'unknown' && s === 'unknown') ? '' : '（'+statuses[s]+'）'));
    const boardEvidence = Array.isArray(row.deviceBoards) ? ['适用板型 '+row.deviceBoards.join('/')] : [];
    const evidence = boardEvidence.concat(Object.entries({...row.requires,...row.pcRequires}).map(([k,v])=>k+'='+v+'（'+check(k,v)+'）')).join('、');
    return {id:row.id,label:row.label,paths,evidence,detail:lines.join(' / ')+(row.grant?' · '+row.grant:'')};
  });
  const labels = {offline:'设备离线',network:'设备联网',pc:'依赖 PC',unknown:'来源待核对'};
  let summary = Array.from(modes,k=>labels[k]).join(' / ') || '本板不支持';
  if (manifest.features.some(row => row.id === 'optional-narration')) summary += ' · 可选朗读';
  const states = rows.flatMap(row=>Object.values(row.paths));
  if (states.includes('offline')) summary += ' · 设备未联网';
  else if (states.includes('unconfirmed')) summary += ' · 板卡待核对';
  else if (states.includes('network-unconfirmed')) summary += ' · 网络待核对';
  return {summary,modes:[...modes],rows};
}
