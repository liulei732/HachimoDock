/**
 * [Input] onSendTest / onCopyDesktopId / onUnbind callbacks plus optional应急 callbacks
 *         onDeviceReturnHome / onForceSyncAppearance (opt-in：caller 传才渲染，
 *         保证既有调用方/测试不破)，以及可选 onUpdateFirmware /
 *         onDiagnostics opt-ins for P4 A/B updates and diagnostics.
 * [Output] Menu mounted into PageShell actions: 发送测试消息 / 复制桌面设备 ID /
 *          [设备返回主屏]? / [重新同步形象]? /
 *          [opt-in] 固件升级 / 设备诊断 / 解绑设备 (danger).
 *          应急两项在物理按键失灵切不回主屏、客户端 UI 与设备形象脱节时使用。
 * [Pos] component node in pc/src/dashboard
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md`.
 */

import React, { useEffect, useRef, useState } from "react";
import { Activity, MoreHorizontal, Send, Copy, Unlink, ArrowUpLeft, RefreshCw, FileUp } from "lucide-react";

export default function DashboardActionsMenu({
  onSendTest,
  onCopyDesktopId,
  onUnbind,
  onDeviceReturnHome,
  onForceSyncAppearance,
  onReconcileAppearance,
  canReconcileAppearance = false,
  onUpdateFirmware,
  onDiagnostics,
}) {
  const [open, setOpen] = useState(false);
  const wrapRef = useRef(null);

  const triggerRef = useRef(null);

  useEffect(() => {
    if (!open) return undefined;
    wrapRef.current?.querySelector('[role="menuitem"]:not(:disabled)')?.focus();
    const handler = (event) => {
      if (wrapRef.current && !wrapRef.current.contains(event.target)) setOpen(false);
    };
    document.addEventListener("mousedown", handler);
    return () => document.removeEventListener("mousedown", handler);
  }, [open]);

  const dispatch = (cb) => () => {
    setOpen(false);
    triggerRef.current?.focus();
    cb?.();
  };

  return (
    <div className="dashboard-actions-menu" ref={wrapRef}
      onBlur={event => {
        if (event.relatedTarget && !event.currentTarget.contains(event.relatedTarget)) setOpen(false);
      }}
      onKeyDown={event => {
        if (!open) {
          if (event.key === "ArrowDown") { event.preventDefault(); setOpen(true); }
          return;
        }
        if (event.key === "Escape") {
          event.preventDefault();
          setOpen(false);
          triggerRef.current?.focus();
          return;
        }
        if (event.key === "Tab") {
          setOpen(false);
          triggerRef.current?.focus();
          return;
        }
        const items = Array.from(wrapRef.current.querySelectorAll('[role="menuitem"]:not(:disabled)'));
        const index = items.indexOf(document.activeElement);
        const next = { ArrowDown: index + 1, ArrowUp: index - 1, Home: 0, End: items.length - 1 }[event.key];
        if (items.length && next !== undefined) {
          event.preventDefault();
          items[(next + items.length) % items.length].focus();
        }
      }}>
      <button
        type="button"
        ref={triggerRef}
        className="btn-secondary btn-sm dashboard-actions-menu__trigger"
        onClick={() => setOpen((prev) => !prev)}
        aria-haspopup="menu"
        aria-expanded={open}
        title="更多操作"
      >
        <MoreHorizontal size={16} /> 设备操作
      </button>
      {open && (
        <div className="dashboard-actions-menu__list" role="menu">
          {onSendTest && <button
            type="button"
            className="dashboard-actions-menu__item"
            role="menuitem"
            onClick={dispatch(onSendTest)}
          >
            <Send size={14} />
            发送测试消息
          </button>}
          {onCopyDesktopId && <button
            type="button"
            className="dashboard-actions-menu__item"
            role="menuitem"
            onClick={dispatch(onCopyDesktopId)}
          >
            <Copy size={14} />
            复制桌面设备 ID
          </button>}
          {onDeviceReturnHome && (
            <button
              type="button"
              className="dashboard-actions-menu__item"
              role="menuitem"
              onClick={dispatch(onDeviceReturnHome)}
              title="请求当前设备返回宠物主屏"
            >
              <ArrowUpLeft size={14} />
              设备返回主屏
            </button>
          )}
          {onReconcileAppearance && (
            <button type="button" className="dashboard-actions-menu__item" role="menuitem"
              disabled={!canReconcileAppearance} onClick={dispatch(onReconcileAppearance)}>
              <RefreshCw size={14} />
              核对形象状态
            </button>
          )}
          {onForceSyncAppearance && (
            <button
              type="button"
              className="dashboard-actions-menu__item"
              role="menuitem"
              onClick={dispatch(onForceSyncAppearance)}
              title="核对设备上的目标形象，已有相同素材包时直接切换，缺少时同步素材"
            >
              <RefreshCw size={14} />
              重新同步形象
            </button>
          )}
          {onUpdateFirmware && (
            <button
              type="button"
              className="dashboard-actions-menu__item"
              role="menuitem"
              onClick={dispatch(onUpdateFirmware)}
            >
              <FileUp size={14} />
              从文件升级 ESP32-P4 固件
            </button>
          )}
          {onDiagnostics && (
            <button
              type="button"
              className="dashboard-actions-menu__item"
              role="menuitem"
              onClick={dispatch(onDiagnostics)}
            >
              <Activity size={14} />
              ESP32-P4 设备诊断
            </button>
          )}
          {onUnbind && <button
            type="button"
            className="dashboard-actions-menu__item dashboard-actions-menu__item--danger"
            role="menuitem"
            onClick={dispatch(onUnbind)}
          >
            <Unlink size={14} />
            解绑设备
          </button>}
        </div>
      )}
    </div>
  );
}
