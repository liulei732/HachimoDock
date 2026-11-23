/**
 * [Input] useDeviceContext for appearances/agentAppearanceMap/agentOptions/currentDisplay/verified USB status/cancellable appearanceSync/refreshAppearances/applyDesktopPet/saveAgentAppearance; useToast for notices.
 * [Output] Agent matrix with USB-priority follow switching, authenticated Wi-Fi appearance sync, cancel controls, and Devin cloud configuration.
 * [Pos] component node in pc/src/dashboard
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md`.
 */

import React, { useCallback, useMemo, useState } from "react";
import {
  CheckCircle,
  Pause,
  Play,
  Repeat2,
  AudioLines,
  Braces,
  Code,
  Loader,
  Terminal,
  UploadCloud,
  X,
  Zap,
} from "lucide-react";
import {
  appearanceById,
  channelLabelForId,
} from "../lib/agent-appearance-config.js";
import { BUILTIN_TERRIER_APPEARANCE_ID } from "../lib/builtin-appearances.js";
import {
  APPEARANCE_SYNC_CANCELLED_MESSAGE,
  APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE,
  CHANNEL_SWITCH_DEVICE_REQUIRED_MESSAGE,
} from "../lib/desktop-pet-assignment.js";
import AppearancePreview from "../AppearancePreview.jsx";
import { resolveDashboardPreviewMedia } from "../lib/appearance-preview.js";
import { useDeviceContext } from "../shell/DeviceContext.jsx";
import { useDialogFocus } from "../component-center/use-dialog-focus.js";
import Card from "../shell/Card.jsx";
import { appearanceStateLabel } from "../lib/appearance-state-label.js";
import Button from "../shell/Button.jsx";
import { useToast } from "../shell/ToastStack.jsx";
import DevinConfigurationButton from "./DevinConfigurationButton.jsx";

const AGENT_ICONS = {
  "claude-code": Code,
  codex: Terminal,
  openclaw: Zap,
  mimocode: Braces,
  workbuddy: Code,
  devin: Code,
  "deepseek-harness": Braces,
};

function appearanceKindLabel(appearance) {
  if (appearance?.type === "codex-import") return "Codex pet";
  if (appearance?.type === "builtin") return "内置";
  return "自定义";
}

function normalizeSyncProgress(progress = {}) {
  const bytesSent = Number(progress.bytesSent || 0);
  const bytesTotal = Number(progress.bytesTotal || 0);
  const hasBytes = Number.isSafeInteger(bytesSent) && Number.isSafeInteger(bytesTotal)
    && bytesSent > 0 && bytesSent <= bytesTotal;
  return {
    text: progress.text || "正在核对设备形象…",
    followOnly: progress.followOnly === true,
    indeterminate: !hasBytes || progress.followOnly === true || progress.indeterminate === true,
    percent: hasBytes ? progress.percent ?? Math.floor(bytesSent / bytesTotal * 100) : null,
  };
}

export default function ChannelMatrixCard({ children, subtitle, actions, onOpenPersona }) {
  const {
    appearances,
    agentAppearanceMap,
    agentOptions,
    currentDisplay,
    usb,
    wifi,
    appearanceSync,
    refreshAppearances,
    applyDesktopPet,
    followAgent,
    cancelAppearanceSync,
    saveAgentAppearance,
  } = useDeviceContext();
  const { push } = useToast();

  const [pickerState, setPickerState] = useState(null); // { agentId }
  const [pendingFollow, setPendingFollow] = useState(null); // { agentId, appearance }
  const syncing = appearanceSync?.pending === true;
  const syncProgress = appearanceSync?.progress
    ? normalizeSyncProgress(appearanceSync.progress)
    : null;

  const visibleAgents = useMemo(() => agentOptions.filter((agent) => agent?.id), [agentOptions]);
  const activeAgentId = currentDisplay.agentId;
  const canFollow = Boolean(usb?.connected || wifi);

  const appearanceForAgent = useCallback(
    (agentId) => {
      const appearanceId = agentAppearanceMap[agentId] || BUILTIN_TERRIER_APPEARANCE_ID;
      return appearanceById(appearances, appearanceId) || appearanceById(appearances, BUILTIN_TERRIER_APPEARANCE_ID) || appearances[0] || null;
    },
    [agentAppearanceMap, appearances],
  );

  const closePicker = useCallback(() => setPickerState(null), []);

  const openPicker = useCallback((agentId) => {
    setPickerState({ agentId });
    refreshAppearances().catch((error) => {
      console.warn("[ChannelMatrixCard] appearance refresh failed", error);
    });
  }, [refreshAppearances]);

  const handleConfirmAppearance = useCallback(async (agentId, appearance) => {
    if (!agentId || !appearance?.id) return;
    const isFollowed = agentId === activeAgentId;
    closePicker();

    if (!isFollowed) {
      saveAgentAppearance(agentId, appearance.id);
      push({
        tone: "success",
        title: `已为 ${channelLabelForId(agentOptions, agentId)} 保存「${appearance.name}」`,
      });
      return;
    }

    try {
      const { notice } = await applyDesktopPet(agentId, appearance, {
        initialProgress: {
          text: `正在核对设备上的「${appearance.name}」…`,
          percent: 0,
          indeterminate: true,
        },
      });
      push({ tone: "success", title: notice || `已同步「${appearance.name}」到设备端` });
    } catch (err) {
      const msg = err?.message || String(err);
      const cancelled = msg === APPEARANCE_SYNC_CANCELLED_MESSAGE;
      const tone =
        cancelled || msg === APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE ? "warning" : "error";
      push({
        tone,
        title: cancelled ? "形象传输已中断" : "更换形象失败",
        message: cancelled ? "已停止本次操作。" : msg,
      });
    }
  }, [activeAgentId, agentOptions, applyDesktopPet, closePicker, push, saveAgentAppearance]);

  const requestFollow = useCallback((agentId) => {
    const appearance = appearanceForAgent(agentId);
    if (!appearance) return;
    setPendingFollow({ agentId, appearance });
  }, [appearanceForAgent]);

  const confirmFollow = useCallback(async () => {
    if (!pendingFollow) return;
    const { agentId, appearance } = pendingFollow;
    setPendingFollow(null);
    const followOnly = !wifi && appearance.id === currentDisplay.appearance?.id;
    try {
      const { notice } = await followAgent(agentId, appearance, {
        initialProgress: followOnly
          ? {
            text: "正在确认…",
            followOnly: true,
          }
          : {
            text: `正在核对「${appearance.name}」并切换跟随…`,
            percent: 0,
            indeterminate: true,
          },
      });
      push({ tone: "success", title: notice || `已跟随 ${channelLabelForId(agentOptions, agentId)}` });
    } catch (err) {
      const msg = err?.message || String(err);
      const cancelled = msg === APPEARANCE_SYNC_CANCELLED_MESSAGE;
      const tone =
        cancelled ||
        msg === APPEARANCE_CHANGE_USB_REQUIRED_MESSAGE ||
        msg === CHANNEL_SWITCH_DEVICE_REQUIRED_MESSAGE
          ? "warning"
          : "error";
      push({
        tone,
        title: cancelled ? "形象传输已中断" : "切换跟随失败",
        message: cancelled ? "本次未切换。" : msg,
      });
    }
  }, [agentOptions, followAgent, currentDisplay.appearance?.id, pendingFollow, push, wifi]);

  const handleCancelAppearanceSync = useCallback(async () => {
    try {
      await cancelAppearanceSync();
    } catch (error) {
      push({
        tone: "error",
        title: "无法中断形象传输",
        message: error?.message || String(error),
      });
    }
  }, [cancelAppearanceSync, push]);

  const appearance = currentDisplay.appearance;
  const media = resolveDashboardPreviewMedia(appearance);
  const activeAgent = visibleAgents.find(agent => agent.id === activeAgentId);
  let pickerHint = "";
  if (syncing) pickerHint = "当前同步完成后可更换形象。";
  else if (!activeAgentId) pickerHint = "先在下方选择要跟随的 Agent。";
  else if (!activeAgent?.detected) pickerHint = "未检测到当前 Agent，请打开客户端后重新扫描。";
  else if (!canFollow) pickerHint = "设备连接 USB 或 Wi-Fi 后可更换形象。";
  const [playing, setPlaying] = useState(() => !window.matchMedia("(prefers-reduced-motion: reduce)").matches);

  return (<>
    {children}
    {appearance && <section className="companion-hero" aria-label="当前跟随的桌宠">
      <div>
        <span className="kicker">{currentDisplay.appearanceUnverified ? "已配置形象 · 待设备确认" : "当前形象"}</span>
        <h2>{appearance.name}</h2>
        {activeAgentId && <div className="meta">正在跟随<strong>{currentDisplay.channelLabel}</strong></div>}
        <div className="companion-hero__actions">
          <button type="button" className="action" onClick={() => onOpenPersona(appearance)} aria-haspopup="dialog">
            <AudioLines size={15} /> 人设和声音
          </button>
          <button type="button" className="companion-hero__switch" onClick={() => openPicker(activeAgentId)}
            disabled={Boolean(pickerHint)} aria-describedby={pickerHint ? "appearance-picker-hint" : undefined}>
            <Repeat2 size={15} /> 更换形象
          </button>
        </div>
        {pickerHint && <p id="appearance-picker-hint" className="muted small">{pickerHint}</p>}
      </div>
      <figure className="screen">
        <div className="film">
          <AppearancePreview key={appearance.id} media={media} playing={playing} className="companion-media" />
          {media.kind === "video" && <button type="button" aria-label={playing ? "暂停预览" : "播放预览"}
            onClick={() => setPlaying(value => !value)}>
            {playing ? <Pause size={14} /> : <Play size={14} />}
          </button>}
        </div>
        <figcaption><span>{media.kind === "video" ? "动作预览" : "形象预览"}</span>
          {media.kind === "video" && <span>{appearanceStateLabel(media.label)}</span>}</figcaption>
      </figure>
    </section>}
    <Card subtitle={subtitle} actions={actions}>
    <div className="channel-matrix" role="region" aria-label="选择跟随的 Agent">
      {syncProgress && (
        <div className="channel-matrix-sync" aria-live="polite">
          <div className="channel-matrix-sync__copy">
            {syncProgress.indeterminate ? <Loader size={15} className="spin" /> : <UploadCloud size={15} />}
            <div>
              <strong>{syncProgress.followOnly ? "正在切换跟随" : "正在切换形象"}</strong>
              <span>{syncProgress.text}</span>
            </div>
          </div>
          {!syncProgress.indeterminate && <div className="channel-matrix-sync__meter">
            <span>{syncProgress.percent}%</span>
            <div className="channel-matrix-sync__bar" role="progressbar"
              aria-label="形象素材传输进度" aria-valuemin={0} aria-valuemax={100}
              aria-valuenow={syncProgress.percent}>
              <span style={{ "--sync-progress": `${syncProgress.percent}%` }} />
            </div>
          </div>}
          {!syncProgress.followOnly && <Button
            className="channel-matrix-sync__cancel"
            variant="danger"
            size="small"
            loading={appearanceSync?.cancelling === true}
            loadingLabel="正在中断…"
            onClick={handleCancelAppearanceSync}
            aria-label={syncProgress.indeterminate ? "取消形象切换" : "中断形象传输"}
          >
            <X size={14} />
            {syncProgress.indeterminate ? "取消切换" : "中断传输"}
          </Button>}
        </div>
      )}

      {visibleAgents.length === 0 ? (
        <div className="channel-matrix__empty">暂时无法读取本机 Agent 列表。</div>
      ) : (
        <div className="channel-matrix__rows">
          {visibleAgents.map((agent) => {
            const Icon = AGENT_ICONS[agent.id] || Code;
            const isFollowed = activeAgentId === agent.id;
            const isDetected = agent.detected === true;
            return (
              <article
                key={agent.id}
                className={`channel-row${isFollowed ? " is-active" : ""}${isDetected ? "" : " is-undetected"}`}
                aria-current={isFollowed ? "true" : undefined}
              >
                <div className="channel-row__channel">
                  <span className="channel-row__icon"><Icon size={16} /></span>
                  <div className="channel-row__label">
                    <strong>{agent.label}</strong>
                    <span className="channel-row__status" title={agent.detail || undefined}>
                      {isDetected ? "已检测" : "未检测到客户端或 CLI"}
                    </span>
                  </div>
                </div>

                {agent.id === "devin" && (
                  <div className="channel-row__settings">
                    <DevinConfigurationButton disabled={!isDetected} />
                  </div>
                )}

                <div className="channel-row__follow">
                  {isFollowed ? (
                    <span className="channel-row__follow-current">已跟随</span>
                  ) : !isDetected ? (
                    <span
                      className="channel-row__unavailable"
                      title="安装对应客户端或 CLI 后重新扫描"
                    >
                      不可用
                    </span>
                  ) : (
                    <button
                      type="button"
                      className="btn-primary btn-sm channel-row__follow-button"
                      onClick={() => requestFollow(agent.id)}
                      disabled={syncing || !canFollow}
                      title={!canFollow ? "用 USB 连接，或与当前设备完成 Wi-Fi 配对" : ""}
                    >
                      跟随
                    </button>
                  )}
                </div>
              </article>
            );
          })}
        </div>
      )}

      {pickerState && (
        <AgentAppearancePickerModal
          appearances={appearances}
          agentLabel={channelLabelForId(agentOptions, pickerState.agentId)}
          selectedAppearanceId={(agentAppearanceMap[pickerState.agentId] || BUILTIN_TERRIER_APPEARANCE_ID)}
          isFollowed={pickerState.agentId === activeAgentId}
          syncing={syncing}
          onClose={closePicker}
          onConfirm={(appearance) => handleConfirmAppearance(pickerState.agentId, appearance)}
        />
      )}

      {pendingFollow && (
        <FollowAgentConfirmModal
          currentLabel={channelLabelForId(agentOptions, activeAgentId)}
          nextLabel={channelLabelForId(agentOptions, pendingFollow.agentId)}
          appearanceName={pendingFollow.appearance?.name || "西高地小狗"}
          reusesCurrentAppearance={pendingFollow.appearance?.id === currentDisplay.appearance?.id}
          wifi={wifi}
          builtin={pendingFollow.appearance?.id === BUILTIN_TERRIER_APPEARANCE_ID}
          syncing={syncing}
          onCancel={() => setPendingFollow(null)}
          onConfirm={confirmFollow}
        />
      )}
    </div>
    </Card>
  </>);
}

function AgentAppearancePickerModal({
  appearances,
  agentLabel,
  selectedAppearanceId,
  isFollowed,
  syncing,
  onClose,
  onConfirm,
}) {
  const [selectedId, setSelectedId] = useState(selectedAppearanceId || BUILTIN_TERRIER_APPEARANCE_ID);
  const selected = appearanceById(appearances, selectedId);
  const canConfirm = Boolean(selected) && !syncing;

  const dialogRef = useDialogFocus(onClose, syncing);
  return (
    <div className="modal-backdrop" onClick={syncing ? undefined : onClose}>
      <div ref={dialogRef} role="dialog" aria-modal="true" aria-label="更换形象" tabIndex={-1}
        className="modal-card modal-card--formosa-picker" onClick={(event) => event.stopPropagation()}>
        <div className="modal-header">
          <div>
            <h3 className="modal-title">更换形象</h3>
            <div className="modal-subtitle">
              {isFollowed
                ? `确认后会把 ${agentLabel} 的新形象实时同步到设备端。`
                : `确认后只保存 ${agentLabel} 的形象选择，不会立即同步设备端。`}
            </div>
          </div>
          <button className="icon-btn" onClick={onClose} aria-label="关闭" disabled={syncing}>
            <X size={16} />
          </button>
        </div>
        <div className="modal-body">
          <div className="formosa-picker__grid">
            {appearances.map((row) => (
              <button
                type="button"
                key={row.id}
                className={`formosa-picker__option${selectedId === row.id ? " is-selected" : ""}`}
                onClick={() => setSelectedId(row.id)}
                disabled={syncing}
              >
                <div className="formosa-picker__stage" aria-hidden="true">
                <AppearancePreview
                  media={resolveDashboardPreviewMedia(row)}
                  className="formosa-picker__media"
                  emptyClassName="formosa-picker__media formosa-picker__media--empty"
                  playing={false}
                />
                </div>
                <div className="formosa-picker__copy">
                  <strong>{row.name}</strong>
                  <span>{appearanceKindLabel(row)}</span>
                </div>
                {selectedId === row.id && <CheckCircle className="formosa-picker__check" size={16} />}
              </button>
            ))}
          </div>
        </div>
        <div className="modal-footer">
          <button className="btn-secondary" type="button" onClick={onClose} disabled={syncing}>
            取消
          </button>
          <button
            className="btn-primary"
            type="button"
            onClick={() => selected && onConfirm(selected)}
            disabled={!canConfirm}
          >
            {syncing ? <Loader size={14} className="spin" /> : <UploadCloud size={14} />}
            确认
          </button>
        </div>
      </div>
    </div>
  );
}

function FollowAgentConfirmModal({
  currentLabel,
  nextLabel,
  appearanceName,
  reusesCurrentAppearance,
  wifi,
  builtin,
  syncing,
  onCancel,
  onConfirm,
}) {
  const dialogRef = useDialogFocus(onCancel, syncing);
  return (
    <div className="modal-backdrop" onClick={syncing ? undefined : onCancel}>
      <div ref={dialogRef} role="dialog" aria-modal="true" aria-label="切换跟随 Agent" tabIndex={-1}
        className="modal-card channel-switch-confirm-modal" onClick={(event) => event.stopPropagation()}>
        <div className="modal-header">
          <div>
            <h3 className="modal-title">切换跟随 Agent</h3>
            <div className="modal-subtitle">设备端同一时间只展示一个 Agent 的实时状态。</div>
          </div>
          <button className="icon-btn" onClick={onCancel} aria-label="关闭" disabled={syncing}>
            <X size={16} />
          </button>
        </div>
        <div className="modal-body channel-switch-confirm-modal__body">
          <div className="message-banner message-banner--muted channel-switch-confirm-modal__message">
            将设备端跟随的 Agent 从 {currentLabel || "当前 Agent"} 切换为 {nextLabel || "目标 Agent"}。
            {wifi?.appearanceCapacity
              ? builtin ? `将核对设备上的内置「${appearanceName}」并切换。`
                : `先核对设备上的「${appearanceName}」。设备未缓存相同版本时才传输素材。`
              : wifi ? "当前固件仅支持通过 Wi-Fi 切换跟随，设备继续使用当前形象。"
                : reusesCurrentAppearance ? `继续使用「${appearanceName}」。` : `同时同步「${appearanceName}」到设备。`}
          </div>
        </div>
        <div className="channel-switch-confirm-modal__actions">
          <button className="btn-secondary" type="button" onClick={onCancel} disabled={syncing}>
            取消
          </button>
          <button className="btn-primary" type="button" onClick={onConfirm} disabled={syncing}>
            {syncing && <Loader size={14} className="spin" />}
            确认跟随
          </button>
        </div>
      </div>
    </div>
  );
}
