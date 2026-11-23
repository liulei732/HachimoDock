/**
 * [Input] Component-library records with `createdAtMs` or ISO `createdAt`.
 * [Output] Recently installed user ordering, followed by the shared builtin sequence.
 * [Pos] component-center library node in pc/src/component-center
 * [Sync] If this file changes, update `pc/src/component-center/.folder.md`.
 */

import {wifiComponentTarget} from '../lib/wifi-device-control.js';
import {compareVersions} from './catalog-state.js';
import factoryConfig from '../../../firmware/factory-config.json' with {type: 'json'};

// Shared with device bootstrap, factory resources and launcher artwork generation.
export const DEFAULT_COMPONENT_ORDER = Object.freeze(factoryConfig.components.ids.filter(id => !factoryConfig.components.firmwareOnlyIds?.includes(id)));
const defaultRanks = new Map(DEFAULT_COMPONENT_ORDER.map((id, index) => [id, index]));

export function sortComponentsByDefaultOrder(components) {
  return [...(components || [])]
    .map((component, index) => ({component, index}))
    .sort((a, b) => (defaultRanks.get(a.component.id) ?? defaultRanks.size)
      - (defaultRanks.get(b.component.id) ?? defaultRanks.size) || a.index - b.index)
    .map(({component}) => component);
}

export function componentCreatedAtMs(component) {
  const numeric = Number(component?.createdAtMs);
  if (Number.isFinite(numeric) && numeric > 0) return numeric;

  const parsed = Date.parse(String(component?.createdAt || ""));
  return Number.isFinite(parsed) ? parsed : 0;
}

export function sortComponentsByCreatedAt(components) {
  return [...(components || [])]
    .map((component, index) => ({ component, index }))
    .sort((left, right) => (
      componentCreatedAtMs(right.component) - componentCreatedAtMs(left.component)
      || left.index - right.index
    ))
    .map(({ component }) => component);
}

export function mergeComponentCatalog(publishedItems, builtins) {
  const publishedIds = new Set(publishedItems.map((item) => item.id));
  const merged = publishedItems.map(local => {
      const builtin=builtins.find(item=>item.id===local.id);
      if(!builtin)return local;
      // Keep both sources available: choosing the newer default never deletes a custom revision.
      const selected=builtin.valid!==false&&(local.valid===false||compareVersions(builtin.version,local.version)===1)?builtin:local;
      return {...selected, isBundledCopy:local.isBundledCopy===true, sourceAlternatives:[local,builtin]};
    });
  return [
    ...merged.filter(item=>!item.isBundledCopy),
    ...sortComponentsByDefaultOrder([...merged.filter(item=>item.isBundledCopy),...builtins.filter((item) => !publishedIds.has(item.id))]),
  ].filter(isComponentVisible);
}

// A withdrawal also applies to packages cached by earlier releases. Match the
// exact package identity, not shared media capabilities or user-authored names.
// Coordinate explicit withdrawals with the packaged factory catalog.
const withdrawnComponentIds = new Set(["upcoming-todos"]); // Merged into hachimi-notes; unified todo storage is retained.
export function isWithdrawnComponent(component) {
  return withdrawnComponentIds.has(component?.id ?? component?.componentId);
}

// Release availability, not deletion: preserve local packages and user data.
export function isComponentVisible(component) {
  return !isWithdrawnComponent(component)
    && component?.id !== "music-player"
    && (component?.mediaSource !== "audio.player" || component?.isLocal === true);
}

export function legacyAudioInstallBlockedReason(component) {
  if (component?.syncRequirements?.blockedReason) return component.syncRequirements.blockedReason;
  if (component?.mediaSource !== "audio.player") return "";
  return "此包使用已停用的旧播放器接口 audio.player。请用 PetUI 的本地媒体服务桥重新打包，再导入和同步。";
}

export function componentInstallBlockedReason(component, usb, wifi, target) {
  const requirements = Object.entries(component?.syncRequirements?.usbCapabilities || {});
  if (!requirements.length) return "";
  let capabilities;
  let transport;
  if (usb?.connected) {
    if (!usb.boardDeviceId) return "USB 设备尚未完成身份握手。";
    capabilities = usb.capabilities;
    transport = "USB";
  } else if (target?.transport === "ssh") {
    return "当前 SSH 连接未协商组件能力，请连接 USB 或受信 Wi-Fi 设备。";
  } else {
    if (!wifi?.wifiConnected) return "无线设备未在线，请连接 USB 或恢复设备 Wi-Fi。";
    if (!wifi.controlTrusted) return "当前无线设备尚未授权控制，请先完成设备信任。";
    if (!wifi.panelControl || !wifi.componentDelivery || !wifi.componentBundle)
      return "当前固件不支持 Wi-Fi 组件分段安装，请更新固件或连接 USB。";
    if (wifi.componentTargetError) return wifi.componentTargetError;
    const snapshot = wifiComponentTarget(wifi);
    if (!snapshot || target?.boardDeviceId !== snapshot.boardDeviceId || target?.bootId !== snapshot.bootId
        || target?.association !== snapshot.association)
      return "未取得当前设备、本次启动和 Wi-Fi 连接的组件能力，请刷新或更新固件。";
    capabilities = snapshot.capabilities;
    transport = "Wi-Fi";
  }
  const missing = requirements.filter(([key, version]) => capabilities?.[key] !== version);
  for (const [key, needed] of Object.entries(component.syncRequirements.minimumLimits || {})) {
    let limit = capabilities?.[key];
    if (limit === undefined) {
      // USB hello v1 and hachi-audio-v2 have published legacy ceilings.
      if (key === "widgetWasmAudioMaxBytes") limit = 512 * 1024;
      else if (transport === "USB") limit = {widgetWasmMaxBytes: 65536, widgetWasmMemoryPages: 2}[key];
    }
    if (!Number.isSafeInteger(limit)) return `当前 ${transport} 设备未报告有效的 ${key}，请刷新能力或更新固件。`;
    if (needed > limit) return `此组件需要 ${key} 至少为 ${needed}，当前 ${transport} 设备仅支持 ${limit}。`;
  }
  return missing.length
    ? `当前 ${transport} 设备未声明组件所需能力：${missing.map(([key, version]) => `${key}=${version}`).join("、")}。请检查固件版本。`
    : "";
}
