/**
 * [Input] Read ChannelMatrixCard.jsx source.
 * [Output] Static Node coverage: default export, visible unavailable-client/CLI Agent rows, removed redundant session controls, followed row state, per-agent appearance saving, picker-open appearance refresh, USB-only followed-agent gating/sync, inline USB sync progress, AgentAppearancePickerModal subcomponent and compact picker modal CSS.
 * [Pos] test node in pc/src/dashboard
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "ChannelMatrixCard.jsx"), "utf8");
const styles = readFileSync(join(here, "../styles.css"), "utf8");

function extractCssRule(css, selector) {
  const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  const match = css.match(new RegExp(`${escaped}\\s*\\{([\\s\\S]*?)\\n\\}`));
  return match ? match[1] : "";
}

test("ChannelMatrixCard has a default export", () => {
  assert.match(source, /export default function ChannelMatrixCard\s*\(/);
});

test("ChannelMatrixCard keeps supported agents visible and marks unavailable clients or CLIs", () => {
  assert.match(source, /visibleAgents/);
  assert.match(source, /agentOptions\.filter\(\(agent\) => agent\?\.id\)/);
  assert.match(source, /visibleAgents\.map\s*\(/);
  assert.match(source, /is-undetected/);
  assert.match(source, /未检测到客户端或 CLI/);
  assert.match(source, /安装对应客户端或 CLI 后重新扫描/);
  assert.doesNotMatch(source, /channel-row__change-inline/);
  assert.match(source, /!isDetected \? \(/);
  assert.match(source, /className="channel-row__unavailable"/);
  assert.match(source, /不可用/);
  assert.match(source, /const canFollow = Boolean\(usb\?\.connected \|\| wifi\)/);
  assert.match(source, /disabled=\{syncing \|\| !canFollow\}/);
  assert.match(source, /followAgent\(agentId, appearance/);
  assert.match(source, /当前固件仅支持通过 Wi-Fi 切换跟随，设备继续使用当前形象/);
  assert.match(source, /将核对设备上的内置/);
  assert.match(source, /设备未缓存相同版本时才传输素材/);
  assert.match(source, /已配置形象 · 待设备确认/);

  const unavailableRule = extractCssRule(styles, ".channel-row.is-undetected");
  assert.match(unavailableRule, /border-style:\s*dashed/);
  assert.doesNotMatch(unavailableRule, /opacity:/);
  const unavailableActionRule = extractCssRule(styles, ".channel-row__unavailable");
  assert.match(unavailableActionRule, /background:\s*var\(--surface-muted\)/);
});

test("ChannelMatrixCard destructures multiple fields from useDeviceContext", () => {
  assert.match(source, /useDeviceContext\s*\(\s*\)/);
  assert.match(source, /appearances/);
  assert.match(source, /agentAppearanceMap/);
  assert.match(source, /agentOptions/);
  assert.match(source, /currentDisplay/);
  assert.match(source, /\busb\b/);
  assert.match(source, /applyDesktopPet/);
  assert.match(source, /saveAgentAppearance/);
  assert.match(source, /refresh/);
});

test("Devin configuration remains available in its row and requires detection", () => {
  assert.match(source, /agent\.id === "devin"[\s\S]*?<DevinConfigurationButton disabled=\{!isDetected\} \/>/);
  const config = readFileSync(join(here, "DevinConfigurationButton.jsx"), "utf8");
  assert.match(config, /disabled=\{disabled\}/);
  assert.match(config, /invoke\("devin_cloud_get_config"\)/);
  assert.match(config, /invoke\("devin_cloud_save_config"/);
  assert.match(config, /invoke\("devin_cloud_test"\)/);
});

test("ChannelMatrixCard applies is-active class to the active channel row", () => {
  assert.match(source, /is-active/);
  assert.match(source, /isFollowed/);
  // Must be conditional based on active channel
  assert.match(source, /isFollowed\s*\?\s*["'].*is-active.*["']/);
});

test("Non-followed agents show a follow button while the active one is marked followed", () => {
  assert.match(source, /已跟随/);
  assert.match(source, /跟随/);
  assert.match(source, /channel-row__follow-current/);
  assert.match(source, /channel-row__follow-button/);
  assert.match(source, /requestFollow\(agent\.id\)/);
});

test("Removes redundant Agent guidance and device-session controls", () => {
  assert.doesNotMatch(source, /选择设备端需要展示实时状态的Agent/);
  assert.doesNotMatch(source, /活跃会话/);
  assert.doesNotMatch(source, /显示会话/);
  assert.doesNotMatch(source, /session-display-setting/);
  assert.doesNotMatch(source, /import Switch/);
  assert.doesNotMatch(styles, /\.session-display-setting/);
});

test("Agent appearance changes save locally unless the agent is currently followed", () => {
  assert.match(source, /BUILTIN_TERRIER_APPEARANCE_ID/);
  assert.match(source, /saveAgentAppearance\(agentId, appearance\.id\)/);
  assert.match(source, /agentId === activeAgentId/);
  assert.match(source, /applyDesktopPet\(agentId, appearance/);
});

test("Opening the appearance picker is immediate while disk records refresh in the background", () => {
  assert.match(source, /const\s+openPicker\s*=\s*useCallback\(\s*\(agentId\)\s*=>/);
  assert.match(source, /setPickerState\(\{\s*agentId\s*\}\);[\s\S]*refreshAppearances\(\)\.catch/);
  assert.doesNotMatch(source, /await\s+refresh\(\)/);
  assert.ok(source.indexOf("setPickerState({ agentId })") < source.indexOf("refreshAppearances().catch"));
  assert.match(source, /onClick=\{\(\)\s*=>\s+openPicker\(activeAgentId\)\}/);
});

test("Follow confirmation describes appearance handling for the active connection", () => {
  assert.match(source, /setPendingFollow\(\{ agentId, appearance \}\)/);
  assert.match(source, /function FollowAgentConfirmModal\s*\(/);
  assert.match(source, /确认跟随/);
  assert.match(source, /同步「\$\{appearanceName\}」到设备/);
  assert.match(source, /当前固件仅支持通过 Wi-Fi 切换跟随，设备继续使用当前形象/);
  assert.match(source, /将核对设备上的内置/);
  assert.match(source, /设备未缓存相同版本时才传输素材/);
  assert.match(source, /已配置形象 · 待设备确认/);
});

test("USB appearance sync progress stays inline and survives dashboard tab unmounts", () => {
  assert.match(source, /appearanceSync/);
  assert.match(source, /const\s+syncing\s*=\s*appearanceSync\?\.pending\s*===\s*true/);
  assert.match(source, /appearanceSync\?\.progress/);
  assert.doesNotMatch(source, /const \[syncProgress,\s*setSyncProgress\]\s*=\s*useState\(null\)/);
  assert.doesNotMatch(source, /const \[syncing,\s*setSyncing\]\s*=\s*useState\(false\)/);
  assert.doesNotMatch(source, /setSyncProgress\(normalizeSyncProgress\(p\)\)/);
  assert.doesNotMatch(source, /onProgress:\s*\(p\)\s*=>\s*push\(\{\s*tone:\s*"info"/);
  assert.match(source, /className="channel-matrix-sync"/);
  assert.match(source, /role="progressbar"/);
  assert.match(source, /aria-valuenow=\{syncProgress\.percent\}/);
  assert.match(source, /cancelAppearanceSync/);
  assert.match(source, /handleCancelAppearanceSync/);
  assert.match(source, /syncProgress\.indeterminate \? "取消形象切换" : "中断形象传输"/);
  assert.match(source, /variant="danger"/);
  assert.match(source, /中断传输/);

  const stripRule = extractCssRule(styles, ".channel-matrix-sync");
  const cancelRule = extractCssRule(styles, ".channel-matrix-sync__cancel");
  const barRule = extractCssRule(styles, ".channel-matrix-sync__bar > span");
  assert.match(stripRule, /display:\s*grid/);
  assert.match(stripRule, /grid-template-columns:[^;]*auto/);
  assert.doesNotMatch(stripRule, /position:\s*(fixed|absolute)/);
  assert.match(cancelRule, /justify-self:\s*end/);
  assert.match(barRule, /width:\s*var\(--sync-progress\)/);
});

test("Follow-only switches are not presented as appearance transfers", () => {
  // Same appearance on the device: no bytes move, so no percent and no abort.
  assert.match(source, /const followOnly = !wifi && appearance\.id === currentDisplay\.appearance\?\.id/);
  assert.match(source, /syncProgress\.followOnly \? "正在切换跟随" : "正在切换形象"/);
  assert.match(source, /\{!syncProgress\.followOnly && <Button/);
  assert.match(source, /text: "正在确认…"/);
  assert.match(source, /initialProgress: followOnly/);
  assert.match(source, /!syncProgress\.indeterminate && <div className="channel-matrix-sync__meter">/);
  assert.match(source, /indeterminate: !hasBytes/);
});

test("AgentAppearancePickerModal subcomponent is declared in the same file", () => {
  assert.match(source, /function AgentAppearancePickerModal\s*\(/);
});

test("FormosaPickerModal CSS stays compact when only a few appearances exist", () => {
  const modalRule = extractCssRule(styles, ".modal-card--formosa-picker");
  const gridRule = extractCssRule(styles, ".formosa-picker__grid");
  const pickerStageRule = extractCssRule(styles, ".formosa-picker__stage");
  const pickerMediaRule = extractCssRule(styles, ".formosa-picker__media");
  assert.match(modalRule, /width:\s*min\(/);
  assert.doesNotMatch(modalRule, /(^|\n)\s*width:\s*100%/);
  assert.match(gridRule, /repeat\(auto-fit,\s*minmax\(140px,\s*180px\)\)/);
  assert.doesNotMatch(gridRule, /repeat\(auto-fill,/);
  assert.match(gridRule, /justify-content:\s*start/);
  assert.match(source, /className="formosa-picker__stage"/);
  assert.match(pickerStageRule, /height:\s*150px/);
  assert.match(pickerStageRule, /place-items:\s*center/);
  assert.match(pickerStageRule, /background:\s*#000/);
  assert.match(pickerMediaRule, /object-fit:\s*contain/);
  assert.match(pickerMediaRule, /object-position:\s*center\s+center/);
  assert.doesNotMatch(source, /className="channel-row__thumb-wrap"/);
  assert.match(source, /className="companion-media"/);
  assert.match(styles, /\.modal-card--formosa-picker\s+\.modal-footer\s*\{/);
});

test("The Agent list does not read, overwrite or expose the saved DeepSeek timeout", () => {
  assert.doesNotMatch(source, /(?:load|save)_dsh_approval_timeout|channel-row__approval-timeout|审批超时时间/);
});
