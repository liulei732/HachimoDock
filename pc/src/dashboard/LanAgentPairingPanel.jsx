import React, { useCallback, useEffect, useLayoutEffect, useRef, useState } from "react";
import { open as openDialog } from "@tauri-apps/plugin-dialog";
import { invoke } from "@tauri-apps/api/core";
import { useDeviceContext } from "../shell/DeviceContext.jsx";
import Card from "../shell/Card.jsx";
import "./wifi-pairing.css";
import { boundedLanRead, matchesUsbPeer, publishLanAgentPairing } from "./lanAgentPairing.js";
import { useExitProtection } from "../lib/exit-protection.js";
import { firmwareUpdateDisposition } from "../lib/firmware-version.js";
import { firmwareAlreadyLatest, firmwareRollback, firmwareUpdateFailureMessage } from "../lib/firmware-update-error.js";

const SCAN_VALID_MS = 150_000; // Native discovery entries expire after 180 seconds.

export default function LanAgentPairingPanel({ usb, active = true, maintenanceRequest = 0 }) {
  const { wifiPairing, pairedStatus, wifiReadError, readWifiStatus, setWifiSettingsOpen } = useDeviceContext();
  const address = typeof wifiPairing === "string" ? wifiPairing : null;
  const [devices, setDevices] = useState([]), [selected, setSelected] = useState(null);
  const [busy, setBusy] = useState(""), [error, setError] = useState("");
  const [scanState, setScanState] = useState(""), [scanError, setScanError] = useState("");
  const [trustError, setTrustError] = useState("");
  const usbConnected = Boolean(usb?.connected);
  const deviceStatus = address && pairedStatus?.address === address ? pairedStatus : null;
  const scope = JSON.stringify([usbConnected, usb?.connectionId, usb?.boardDeviceId, usb?.bootId,
    address, deviceStatus?.boardDeviceId, deviceStatus?.bootId, deviceStatus?.controlTrusted]);
  const generation = useRef(0);
  // A reboot or OTA can take the same peer offline and change its boot ID.
  // Keep its completion message across that transition, scoped to the actual peer.
  const lastPeer = useRef(null);
  const feedbackBoard = deviceStatus?.boardDeviceId ||
    (lastPeer.current?.address === address ? lastPeer.current?.id : null);
  const feedbackScope = JSON.stringify([address, feedbackBoard, usbConnected, usb?.connectionId, usb?.boardDeviceId]);
  const feedbackGeneration = useRef(0);
  const scanGeneration = useRef(0);
  const scanExpiry = useRef(null);
  const trustAttempt = useRef(null);
  const matched = matchesUsbPeer(usb, deviceStatus);
  const needsTrust = matched && deviceStatus.rebootSupported && !deviceStatus.controlTrusted;
  useEffect(() => {
    setWifiSettingsOpen(active);
    return () => setWifiSettingsOpen(false);
  }, [active, setWifiSettingsOpen]);
  const [deliveryMessage, setDeliveryMessage] = useState("");
  const [rebootMessage, setRebootMessage] = useState("");
  const [otaMessage, setOtaMessage] = useState("");
  const [rollbackBoardId, setRollbackBoardId] = useState(null);
  const maintenance = useRef(null);
  const openedMaintenance = useRef(0);
  useEffect(() => {
    if (!active || maintenanceRequest <= openedMaintenance.current || !maintenance.current) return;
    openedMaintenance.current = maintenanceRequest;
    maintenance.current.open = true;
    maintenance.current.querySelector("summary")?.focus({ preventScroll: true });
    maintenance.current.scrollIntoView({ block: "center" });
  }, [active, address, maintenanceRequest]);
  const firmwareDisposition = firmwareUpdateDisposition(deviceStatus?.firmware, deviceStatus?.bundledFirmwareVersion);
  const rollbackBlocked = firmwareDisposition !== "latest" && Boolean(deviceStatus &&
    (deviceStatus.bundledFirmwareRollbackBlocked || rollbackBoardId === deviceStatus.boardDeviceId));
  const canMutateOverLan = !usbConnected && Boolean(deviceStatus?.controlTrusted);
  const savedPeerUnpaired = address && !deviceStatus && devices.some(device =>
    device.ip === address && device.ready && device.paired === false);

  useLayoutEffect(() => {
    lastPeer.current = { address, id: feedbackBoard };
  }, [address, feedbackBoard]);
  useLayoutEffect(() => {
    setTrustError("");
    return () => { generation.current += 1; };
  }, [scope, active]);
  useLayoutEffect(() => {
    setError("");
    setDeliveryMessage("");
    setRebootMessage("");
    setOtaMessage("");
    return () => { feedbackGeneration.current += 1; };
  }, [feedbackScope]);
  useExitProtection(busy === "pair" ? "正在完成设备配对" : busy === "install" ? "正在通过 Wi-Fi 安装组件" : busy === "reboot" ? "正在确认设备重启" : busy === "ota" ? "正在通过 Wi-Fi 升级设备" : "");
  const scan = useCallback(async () => {
    const id = ++scanGeneration.current;
    clearTimeout(scanExpiry.current);
    setDevices([]);
    setSelected(null);
    setScanError("");
    setScanState("scanning");
    try {
      const found = await boundedLanRead(invoke("lan_agent_discover"), "发现设备超时，请重新扫描。", 8000);
      if (id !== scanGeneration.current) return;
      setDevices(found);
      setScanState("done");
      scanExpiry.current = setTimeout(() => {
        setSelected(null);
        setDevices([]);
        setScanState("expired");
      }, SCAN_VALID_MS);
    } catch (e) {
      if (id !== scanGeneration.current) return;
      setScanError(e?.message || String(e));
      setScanState("failed");
    }
  }, []);
  useEffect(() => {
    if (active) void scan();
    return () => {
      scanGeneration.current += 1;
      clearTimeout(scanExpiry.current);
    };
  }, [active, address, feedbackBoard, usb?.connectionId, usb?.boardDeviceId, scan]);

  const authorize = useCallback(async () => {
    if (!needsTrust) return;
    const owner = generation.current;
    trustAttempt.current = scope;
    setBusy("trust");
    setTrustError("");
    try {
      await invoke("lan_trust_usb", { target: { boardDeviceId: deviceStatus.boardDeviceId,
        bootId: deviceStatus.bootId, connectionId: usb.connectionId } });
      if (owner !== generation.current) return;
      const confirmed = await readWifiStatus(true);
      if (owner === generation.current && !confirmed?.controlTrusted) {
        setTrustError("无线控制授权尚未确认，请重试。");
      }
    } catch (e) {
      if (owner === generation.current) setTrustError(e?.message || String(e));
    } finally {
      setBusy("");
    }
  }, [needsTrust, scope, readWifiStatus, deviceStatus, usb?.connectionId]);
  useEffect(() => {
    if (active && needsTrust && !busy && trustAttempt.current !== scope) void authorize();
  }, [active, needsTrust, busy, scope, authorize]);
  const pair = async () => {
    if (!selected || busy) return;
    const owner = generation.current;
    setBusy("pair");
    setError("");
    try {
      const saved = await invoke("lan_agent_pair_confirm", { id: selected.id, ip: selected.ip });
      publishLanAgentPairing(saved);
      if (owner === generation.current) {
        setSelected(null);
        setDevices(current => current.map(device => device.id === selected.id && device.ip === selected.ip
          ? { ...device, ready: false, paired: true } : device));
      }
    } catch (e) {
      if (owner === generation.current) {
        setSelected(null);
        setError(e?.message || String(e));
      }
    } finally {
      setBusy("");
    }
  };
  const checkStatus = async () => {
    setBusy("status");
    setError("");
    try { await readWifiStatus(); }
    catch { /* The shared snapshot owns read errors and recovery. */ }
    finally { setBusy(""); }
  };
  const chooseComponent = async () => {
    if (!canMutateOverLan || busy || !deviceStatus?.bootId || !deviceStatus?.boardDeviceId) return;
    const owner = generation.current;
    setBusy("install");
    setError("");
    setDeliveryMessage("");
    try {
      const path = await openDialog({ filters: [{ name: "PetUI 组件", extensions: ["hachiapp", "clawpkg", "zip"] }] });
      if (typeof path !== "string" || owner !== generation.current) return;
      const preview = await invoke("lan_component_preview", {path});
      if (owner !== generation.current) return;
      await invoke("lan_component_install", { path, ...preview,
        expectedBoardDeviceId: deviceStatus.boardDeviceId, expectedBootId: deviceStatus.bootId,
        expectedAssociation: deviceStatus.componentTarget?.association });
      if (owner === generation.current) setDeliveryMessage("设备已提交安装。");
    } catch (e) {
      if (owner === generation.current) setError(e?.message || String(e));
    } finally {
      setBusy("");
    }
  };
  const rebootDevice = async () => {
    const expectedBootId = deviceStatus?.bootId;
    if (!canMutateOverLan || busy || !expectedBootId) return;
    const owner = feedbackGeneration.current;
    setBusy("reboot");
    setError("");
    setRebootMessage("");
    try {
      const result = await invoke("lan_reboot", {expectedBootId});
      if (owner !== feedbackGeneration.current) return;
      if (result.observed) {
        setRebootMessage(result.scheduled ? "设备已重启并重新上线。" : "已验证设备重新上线，启动 ID 已改变。");
      } else {
        setRebootMessage(result.scheduled
          ? "设备已安排重启，尚未读到新的启动 ID。"
          : "重启结果未确认。请查看设备，或重新读取状态。");
      }
    } catch (e) {
      if (owner === feedbackGeneration.current) setError(e?.message || String(e));
    } finally {
      void readWifiStatus(true).catch(() => {});
      setBusy("");
    }
  };
  const updateFirmware = async () => {
    const expectedBoardDeviceId = deviceStatus?.boardDeviceId;
    const expectedBootId = deviceStatus?.bootId;
    if (!canMutateOverLan || busy || rollbackBlocked || !expectedBoardDeviceId || !expectedBootId) return;
    const owner = feedbackGeneration.current;
    setBusy("ota");
    setError("");
    setOtaMessage("");
    try {
      const { version } = await invoke("lan_update_bundled_firmware", {expectedBoardDeviceId, expectedBootId});
      if (owner === feedbackGeneration.current) setOtaMessage(`已通过 Wi-Fi 升级至 ${version}。`);
    } catch (e) {
      if (owner !== feedbackGeneration.current) return;
      if (firmwareAlreadyLatest(e)) setOtaMessage("设备固件已是最新版本。");
      else {
        if (firmwareRollback(e)) setRollbackBoardId(expectedBoardDeviceId);
        setError(firmwareUpdateFailureMessage(e, { wireless: true }));
      }
    } finally {
      void readWifiStatus(true).catch(() => {});
      setBusy("");
    }
  };
  const forget = async () => {
    const owner = generation.current;
    setBusy("forget");
    setError("");
    try {
      await invoke("lan_agent_forget");
      publishLanAgentPairing(null);
    } catch {
      if (owner === generation.current) setError("无法移除本机配对，请重试。");
    } finally {
      setBusy("");
    }
  };
  return <Card title="Wi-Fi 设备配对" subtitle="电脑与设备位于同一局域网时，可通过 Wi-Fi 同步。USB 接入时优先使用 USB。">
    <div className="wifi-pairing">
      <section className="wifi-pairing__current" aria-label="当前配对">
        <div>
          <h3>{address && !deviceStatus ? "本机配对记录" : "当前配对"}</h3>
          {address ? <>
            <strong>{deviceStatus?.boardDeviceId || address}</strong>
            <p className="muted small">{[
              deviceStatus?.boardDeviceId && address,
              deviceStatus?.firmware,
              deviceStatus && (deviceStatus.controlTrusted ? "已授权无线控制" : "只读连接"),
              deviceStatus && (deviceStatus.wifiConnected ? "设备 Wi-Fi 已连接" : "设备 Wi-Fi 未连接"),
              deviceStatus && (deviceStatus.clockSynchronized ? "已校时" : "待校时"),
            ].filter(Boolean).join(" · ") || (savedPeerUnpaired
              ? "本机有旧记录，设备报告尚未配对。可在下方重新配对。"
              : "已保存本机配对，尚未验证设备连接。")}</p>
          </> : <p role="status">{wifiPairing === false ? "配对记录暂时无法读取，正在重试。" : wifiPairing === null ? "尚未配对，从下方选择设备。" : "正在读取配对记录…"}</p>}
        </div>
        {(address || wifiReadError) && <button type="button" className="btn-secondary btn-sm" disabled={!!busy} onClick={checkStatus}>
          {busy === "status" ? "正在读取…" : "刷新状态"}
        </button>}
      </section>

      <section aria-label="附近设备">
        <div className="wifi-pairing__section-head">
          <h3>附近设备</h3>
          <button type="button" className="btn-secondary btn-sm" disabled={!!busy || scanState === "scanning"} onClick={() => { setError(""); void scan(); }}>
            {scanState === "scanning" ? "正在发现…" : "重新扫描"}
          </button>
        </div>
        <div className="wifi-pairing__list" role="radiogroup" aria-label="选择要配对的设备">
          {devices.map(device => {
            const isCurrent = Boolean(address === device.ip && deviceStatus?.boardDeviceId === device.id);
            const label = isCurrent ? "当前配对" : device.ready ? (device.ip === address ? "可重新配对" : "可配对")
              : !device.supportsAutomatic ? "需升级固件"
                : device.paired ? "已有配对" : "暂不可配对";
            return <label key={`${device.id}-${device.ip}`} className="wifi-pairing__device">
              <input type="radio" name="lan-device" disabled={!!busy || !device.ready || isCurrent}
                checked={selected?.id === device.id && selected?.ip === device.ip}
                onChange={() => setSelected(device)} />
              <span><strong>{device.id}</strong><small>{device.ip}</small></span>
              <span className="api-settings__status">{label}</span>
            </label>;
          })}
        </div>
        {scanState === "expired" && <p role="status">扫描结果已过期，请重新扫描。</p>}
        {scanError && <p role="alert">{scanError}</p>}
        {scanState === "done" && !devices.length && <p role="status" className="muted">暂未发现设备。确认双方连在同一局域网，再重新扫描。</p>}
        {selected && <button type="button" className="btn-primary btn-sm" disabled={!!busy} onClick={pair}>确认配对</button>}
        {busy === "pair" && <p role="status">正在建立连接并保存配对。</p>}
      </section>

      <details>
        <summary>如何连接设备</summary>
        <ol>
          <li>在设备上长按 3 键，或从组件中心打开系统设置。</li>
          <li>进入 Wi-Fi 配置，选择网络并输入密码。</li>
          <li>让电脑加入同一局域网，在上方选择设备并确认配对。下次启动时，没有 USB 连接就会自动连接上次配对的设备。</li>
        </ol>
      </details>

      {(address || maintenanceRequest > 0) && <details ref={maintenance}>
        <summary>设备维护与配对管理</summary>
        {deviceStatus && <p role="status">{deviceStatus.controlTrusted
          ? "此电脑已获无线控制授权。"
          : deviceStatus.rebootSupported ? "通过 USB 连接这台设备可授权无线控制。" : "当前固件仅支持读取无线状态。"}</p>}
        {deviceStatus && usbConnected && <p>{matched
          ? "当前通过 USB 控制设备，断开后可使用已授权的无线控制。"
          : "当前 USB 连接与 Wi-Fi 配对目标尚未匹配。请核对设备。"}</p>}
        {needsTrust && <button type="button" className="btn-secondary btn-sm" disabled={!!busy} onClick={authorize}>
          {busy === "trust" ? "正在授权…" : "通过 USB 授权无线控制"}
        </button>}
        <div className="wifi-pairing__actions">
          {canMutateOverLan && deviceStatus?.rebootSupported &&
            <button type="button" className="btn-secondary btn-sm" disabled={!!busy || !deviceStatus.bootId} onClick={rebootDevice}>
              {busy === "reboot" ? "等待设备上线…" : "无线重启"}
            </button>}
          {canMutateOverLan && deviceStatus?.wirelessOta && firmwareDisposition !== "latest" && !rollbackBlocked &&
            <button type="button" className="btn-secondary btn-sm" disabled={!!busy || !deviceStatus.bootId} onClick={updateFirmware}>
              {busy === "ota" ? "正在升级…" : "无线更新固件"}
            </button>}
          {rollbackBlocked && <span role="status">固件已回退</span>}
          {canMutateOverLan && deviceStatus?.componentDelivery &&
            <button type="button" className="btn-secondary btn-sm" disabled={!!busy} onClick={chooseComponent}>
              {busy === "install" ? "正在安装…" : "无线安装组件"}
            </button>}
          {address && <button type="button" className="btn-secondary btn-sm" disabled={!!busy} onClick={forget}>忘记本机配对</button>}
        </div>
      </details>}
      {rebootMessage && <p role="status">{rebootMessage}</p>}
      {otaMessage && <p role="status">{otaMessage}</p>}
      {deliveryMessage && <p role="status">{deliveryMessage}</p>}
      {needsTrust && trustError && <p className="api-settings__result is-error" role="alert">{trustError}</p>}
      {wifiReadError && <p className="api-settings__result is-error" role="status">{wifiReadError}</p>}
      {error && <p className="api-settings__result is-error" role="alert">{error}</p>}
    </div>
  </Card>;
}
