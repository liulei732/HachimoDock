/** [Input] Public records, local library and live device inventory.
 * [Output] Honest version states and bounded local-only favorites; no credentials.
 * [Sync] Keep component-center/.folder.md and community-model.test.js in sync. */
export const COMMUNITY_TABS = [
  ["local", "我的组件"],
  ["discover", "社区广场"],
  ["favorites", "我的收藏"],
  ["created", "我的创作"],
];
export const COMMUNITY_CATEGORIES = [
  ["", "全部"],
  ["hachimi", "哈基米专属"],
  ["game", "小游戏"],
  ["productivity", "效率"],
  ["tool", "工具"],
  ["relax", "放松"],
  ["life", "生活"],
];
export const FAVORITES_KEY = "pet-manager:community-favorites:v1";
export const NAME_KEY = "pet-manager:community-display-name:v1";
export const CONTACT_URL = "https://github.com/YizhengWw/HachimoDock/issues";
import {compareVersions} from './catalog-state.js';
import {isWithdrawnComponent} from './library-order.js';
export {compareVersions};
export function versionState(work, local, inventory) {
  const installed =
    inventory?.freshness === "live"
      ? inventory.items?.find((item) => item.id === work?.id)
      : null;
  const difference = compareVersions(local?.version, work?.version);
  const differentContent = Boolean(local?.versionHash && work?.contentHash && difference === 0 && !work.contentHash.startsWith(local.versionHash));
  const officialBundled = work?.official === true && local?.isBundledCopy === true;
  const contentConflict = differentContent && !officialBundled;
  return {
    localLabel: local ? `本机 ${local.version || "版本未知"}` : "尚未下载",
    deviceLabel:
      inventory?.freshness !== "live"
        ? "设备未核对"
        : installed
          ? `设备 ${installed.version || "版本未核验"}`
          : "未安装到设备",
    updateAvailable: Boolean(local && (difference === -1 || (differentContent && officialBundled))),
    localNewer: Boolean(local && difference === 1),
    contentConflict,
    versionUnknown: Boolean(local && difference === null),
    installed: Boolean(installed),
  };
}
export function readFavorites(storage) {
  try {
    const entries = JSON.parse(storage.getItem(FAVORITES_KEY) || "[]");
    return Array.isArray(entries)
      ? entries
          .filter((v) => v && /^[a-z][a-z0-9_-]{0,43}$/.test(v.id) && !isWithdrawnComponent(v))
          .slice(0, 200)
      : [];
  } catch {
    return [];
  }
}
export function updateFavorites(entries, work) {
  if (isWithdrawnComponent(work)) return entries;
  if (entries.some((v) => v.id === work.id))
    return entries.filter((v) => v.id !== work.id);
  // Public metadata only; never serialize package paths, private settings or tokens.
  const { id, name, version, author, summary, category, covers, available } =
    work;
  return [
    { id, name, version, author, summary, category, covers, available },
    ...entries,
  ].slice(0, 200);
}
export function publicationCandidates(items, provenance, owned) {
  if (!Array.isArray(provenance?.reservedIds) || !provenance.downloaded) return [];
  const candidates = new Map();
  for (const item of items) {
    for (const source of item.sourceAlternatives || [item]) {
      const {id, libraryPath} = source;
      if (!source.isLocal || !libraryPath || source.isBundledCopy || isWithdrawnComponent(source)
          || !/^[a-z][a-z0-9_-]{0,43}$/.test(id || "") || /^(official-|hachimo-)/.test(id)
          || provenance.reservedIds.includes(id)
          || (Object.hasOwn(provenance.downloaded, id) && !Object.hasOwn(owned, id))) continue;
      candidates.set(libraryPath, source);
    }
  }
  return [...candidates.values()];
}

export function publicationReceiptError(receipt, id, version) {
  if (receipt?.canPublish === false) return receipt.warning || "社区未通过此组件的发布预检。";
  if (receipt?.serverChecked !== true || receipt.canPublish !== true)
    return "尚未取得社区预检通过回执，请重新检查。";
  if (receipt.id !== id || !receipt.version || (version && receipt.version !== version)
      || !/^[a-f0-9]{64}$/.test(receipt.contentHash || "") || !Array.isArray(receipt.files)
      || !receipt.files.length || !receipt.files.every(name => typeof name === "string"))
    return "预检回执与所选本地组件不匹配，请重新检查。";
  return "";
}

export function publicationStatus(work) {
  if (work.status === "pending") return "待审核";
  if (work.status === "rejected") return "需修改";
  return work.available ? "已上架" : "已下架";
}
export function readableBytes(bytes) {
  return Number(bytes) >= 1048576
    ? `${(bytes / 1048576).toFixed(1)} MB`
    : `${Math.max(1, Math.round(Number(bytes || 0) / 1024))} KB`;
}
