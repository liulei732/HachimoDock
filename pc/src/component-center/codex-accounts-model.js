// Use the declared provider, never a component name or an unrelated stock ID.
export function hasCodexAccounts(component) {
  return component?.pcApplication?.content?.some(plan => Object.values(plan?.sources || {})
    .some(source => source?.kind === 'local-telemetry' && source.source === 'codex.accounts')) === true;
}
export function canDisplayEmail(runtime, email) {
  if (!email) return false;
  const fonts = runtime?.surface?.fonts;
  if (!Array.isArray(fonts) || !fonts.length) return /^[\x20-\x7e]+$/.test(email);
  const glyphs = new Set(fonts.flatMap(font => Array.from(font.chars || '')));
  return Array.from(email).every(char => glyphs.has(char));
}
export function displayChange(settings, slot, checked) {
  return {operation:'configure', revision:settings.revision, order:[...settings.order],
    visible:checked ? [...new Set([...settings.visible,slot])] : settings.visible.filter(s=>s!==slot)};
}
export function moveAccount(settings, slot, direction) {
  const order=[...settings.order], index=order.indexOf(slot), next=index+direction;
  if (![-1,1].includes(direction) || index<0 || next<0 || next>=order.length) return null;
  [order[index],order[next]]=[order[next],order[index]];
  return {operation:'configure',revision:settings.revision,order,visible:[...settings.visible]};
}
export function loginPhase(login) {
  return ({preparing:'准备登录中…',pending:'等待浏览器完成登录',committing:'正在保存登录',
    completed:'登录已完成',cancelling:'正在取消登录…',cancelled:'登录已取消',error:login?.error||'登录未完成'})[login?.phase] || '';
}
export function accountState(account) {
  if (!account.loginSaved) return '尚未登录';
  if (account.status?.includes('重新登录')) return '需要重新授权';
  if (account.status === '查询中') return '正在刷新额度';
  if (account.status?.startsWith('更新')) return '可用';
  if (account.status?.includes('过期') || account.observedAt) return '最近结果已过期';
  return '登录信息已保存 · 额度待刷新';
}
