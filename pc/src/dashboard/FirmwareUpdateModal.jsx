/**
 * [Input] open/onClose props, expected board identity, current ESP32-P4
 *         firmware version, native file picker, usb_update_firmware command,
 *         and firmware progress events.
 * [Output] A/B firmware update dialog with .bin selection, acknowledged upload
 *          progress, reboot validation stages, and only reconnect-verified
 *          valid slot/version success.
 * [Pos] component node in pc/src/dashboard
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md`.
 */

import { useDialogFocus } from "../component-center/use-dialog-focus.js";
import React, { useEffect, useLayoutEffect, useMemo, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { open as openDialog } from "@tauri-apps/plugin-dialog";
import { CheckCircle2, FileUp, LoaderCircle, X } from "lucide-react";

import { useExitProtection } from "../lib/exit-protection.js";

const INITIAL_PROGRESS = { stage: "idle", bytesSent: 0, bytesTotal: 0, percent: 0 };

const STAGE_LABELS = {
  begin: "正在创建备用固件槽",
  recover: "旧版固件中断，正在自动恢复升级",
  upload: "正在传输并逐块校验",
  verify: "正在校验镜像完整性",
  reboot: "镜像完整性通过，正在等待设备重启",
  validate: "设备已重连，正在确认新版本有效",
  font: "固件已验证，正在同步气泡字库",
};

function fileNameFromPath(path) {
  return String(path || "").split(/[\\/]/).filter(Boolean).at(-1) || "";
}

function formatBytes(value) {
  const bytes = Number(value) || 0;
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

export default function FirmwareUpdateModal({
  open,
  onClose,
  usb,
  wifi,
  expectedWifiBootId,
  expectedBoardDeviceId,
}) {
  const updating = useRef(null);
  const dialogSession = useRef(0);
  const pickerSession = useRef(0);
  const [choosing, setChoosing] = useState(false);
  const [firmwarePath, setFirmwarePath] = useState("");
  const [progress, setProgress] = useState(INITIAL_PROGRESS);
  const [running, setRunning] = useState(null);
  const [result, setResult] = useState(null);
  const [error, setError] = useState("");

  const otherBoard = Boolean(usb?.connected && usb.boardDeviceId && usb.boardDeviceId !== expectedBoardDeviceId);
  const wireless = Boolean(expectedWifiBootId);
  const matchingConnection = wireless
    ? !usb?.connected && wifi?.wirelessOta && wifi?.controlTrusted && wifi?.bootId === expectedWifiBootId && wifi?.boardDeviceId === expectedBoardDeviceId
    : usb?.connected && usb.connectionId && usb.boardDeviceId === expectedBoardDeviceId;
  const canSelect = Boolean(open && expectedBoardDeviceId && matchingConnection);
  const pending = running && running.session === dialogSession.current;
  useExitProtection(running ? "固件升级中，请等待设备重新连接并校验完成" : "");

  useLayoutEffect(() => {
    setProgress(INITIAL_PROGRESS);
    setResult(null);
    setError("");
    return () => { dialogSession.current++; };
  }, [open, expectedBoardDeviceId, otherBoard]);

  // A chosen file belongs to the USB session that presented the picker. Once
  // submitted, native owns reconnection and validates the new boot itself.
  useLayoutEffect(() => {
    setChoosing(false);
    setFirmwarePath("");
    return () => { pickerSession.current++; };
  }, [open, expectedBoardDeviceId, usb?.boardDeviceId, usb?.connected, usb?.connectionId, usb?.bootId, expectedWifiBootId, canSelect]);

  useEffect(() => {
    if (!open) return;
    let cancelled = false;
    let unlisten;
    const receiveProgress = ({ payload }) => {
      const owner = updating.current;
      if (cancelled || !owner || owner.session !== dialogSession.current || payload?.updateId !== owner.id) return;
      setProgress({
        stage: String(payload.stage || "upload"),
        bytesSent: Number(payload.bytesSent) || 0,
        bytesTotal: Number(payload.bytesTotal) || 0,
        percent: Math.max(0, Math.min(100, Number(payload.percent) || 0)),
      });
    };
    const subscription = wireless ? listen("lan-firmware-update-progress", receiveProgress) : listen("usb-firmware-update-progress", receiveProgress);
    subscription.then(stop => {
      if (cancelled) stop();
      else unlisten = stop;
    }).catch(() => {}); // The command result still reports final validation.
    return () => {
      cancelled = true;
      unlisten?.();
    };
  }, [open, wireless]);

  const displayedPath = pending ? running.firmwarePath : firmwarePath;
  const selectedName = useMemo(() => fileNameFromPath(displayedPath), [displayedPath]);

  const selectFirmware = async () => {
    if (!canSelect || updating.current) return;
    const session = ++pickerSession.current;
    setChoosing(true);
    setError("");
    try {
      const selected = await openDialog({
        multiple: false,
        directory: false,
        filters: [{ name: "ESP32-P4 firmware", extensions: ["bin"] }],
      });
      if (session === pickerSession.current && typeof selected === "string") {
        setFirmwarePath(selected);
        setResult(null);
        setProgress(INITIAL_PROGRESS);
      }
    } catch (problem) {
      if (session === pickerSession.current) setError(String(problem?.message || problem));
    } finally {
      if (session === pickerSession.current) setChoosing(false);
    }
  };

  const startUpdate = async () => {
    if (!canSelect || !firmwarePath || choosing || updating.current) return;
    const owner = { session: dialogSession.current, id: crypto.randomUUID(), firmwarePath };
    updating.current = owner;
    setRunning(owner);
    setResult(null);
    setError("");
    setProgress({ ...INITIAL_PROGRESS, stage: "begin" });
    try {
      const nextResult = wireless ? await invoke("lan_update_firmware_file", {
        firmwarePath,
        expectedBoardDeviceId,
        expectedBootId: expectedWifiBootId,
        updateId: owner.id,
      }) : await invoke("usb_update_firmware", {
        firmwarePath,
        expectedBoardDeviceId,
        expectedConnectionId: usb.connectionId,
        updateId: owner.id,
      });
      if (owner.session !== dialogSession.current) return;
      setResult(nextResult);
      setProgress((current) => ({ ...current, stage: "reboot", percent: 100 }));
    } catch (updateError) {
      if (owner.session === dialogSession.current) setError(typeof updateError === "string" ? updateError : String(updateError));
    } finally {
      updating.current = null;
      setRunning(null);
    }
  };


  const close = () => {
    if (updating.current?.session === dialogSession.current) return;
    pickerSession.current++;
    onClose?.();
  };
  const dialogRef = useDialogFocus(close, Boolean(pending) || choosing, open);
  if (!open) return null;
  const stageLabel = STAGE_LABELS[progress.stage] || "等待选择固件";

  return (
    <div className="modal-backdrop" role="presentation" onMouseDown={close}>
      <div
        ref={dialogRef} tabIndex={-1}
        className="modal-card firmware-update-modal"
        role="dialog"
        aria-modal="true"
        aria-label="ESP32-P4 固件升级"
        onMouseDown={(event) => event.stopPropagation()}
      >
        <div className="modal-header">
          <div>
            <h3 className="modal-title">ESP32-P4 固件升级</h3>
            <p className="firmware-update-modal__subtitle">
              目标设备：<code>{expectedBoardDeviceId}</code> · 当前版本：<code>{canSelect ? (wireless ? wifi.firmware : usb.firmware) || "未知" : "未知"}</code>
            </p>
          </div>
          <button type="button" className="icon-btn" onClick={close} disabled={pending} aria-label="关闭">
            <X size={16} />
          </button>
        </div>

        <div className="modal-body firmware-update-modal__body">
          <button
            type="button"
            className="firmware-update-modal__picker"
            onClick={selectFirmware}
            disabled={!canSelect || Boolean(running) || choosing}
          >
            <FileUp size={20} />
            <span>
              <strong>{selectedName || "选择 firmware.bin"}</strong>
              <small>{displayedPath || "仅接受 ESP-IDF 生成的 .bin 应用镜像"}</small>
            </span>
          </button>

          {(pending || result) && (
            <div className="firmware-update-modal__progress" aria-live="polite">
              <div className="firmware-update-modal__progress-meta">
                <span>{pending ? <LoaderCircle className="spin" size={15} /> : <CheckCircle2 size={15} />}</span>
                <strong>{result ? "新固件已在设备上确认有效" : stageLabel}</strong>
                <span>{progress.percent}%</span>
              </div>
              <div className="firmware-update-modal__progress-track">
                <span style={{ width: `${progress.percent}%` }} />
              </div>
              {progress.bytesTotal > 0 && <small>{formatBytes(progress.bytesSent)} / {formatBytes(progress.bytesTotal)}</small>}
            </div>
          )}

          {result && (
            <div className="firmware-update-modal__result" data-tone="success">
              设备已重连，新版本 <code>{result.version || "未知"}</code> 已在
              <code>{result.targetPartition || "备用槽"}</code> 确认为 <code>{result.imageState || "valid"}</code>。
            </div>
          )}
          {error && <div className="firmware-update-modal__result" data-tone="error">{error}</div>}
          {otherBoard && <p role="status">USB 已切换到另一台设备。请关闭弹窗，在目标设备上重新选择固件。</p>}
          {!canSelect && !otherBoard && !pending && !result && <p role="status">请重新连接目标设备，再选择固件。</p>}
          {running && !pending && <p role="status">正在等待上一次升级结束。</p>}
          {canSelect && !running && !result && !error && (
            <p className="firmware-update-modal__notice">
              升级写入备用槽，不会覆盖当前可启动版本。传输开始后请保持{wireless ? " Wi-Fi " : " USB "}连接和设备供电。
            </p>
          )}
        </div>

        <div className="modal-footer firmware-update-modal__footer">
          <button type="button" className="btn-secondary" onClick={close} disabled={pending}>
            {result ? "完成" : "取消"}
          </button>
          {!result && (
            <button
              type="button"
              className="btn-primary"
              onClick={startUpdate}
              disabled={!canSelect || !firmwarePath || Boolean(running) || choosing}
            >
              {pending ? "升级中..." : "开始升级"}
            </button>
          )}
        </div>
      </div>
    </div>
  );
}
