import React, { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import "./stock-watchlist.css";

export function hasPythonPlugin(component) {
  const sources = component.pcApplication?.content?.[0]?.sources;
  return Object.values(sources ?? {}).some(source => source?.kind === "python");
}

export default function PythonPlugin({ component }) {
  const { id, versionHash } = component;
  const [status, setStatus] = useState(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");

  useEffect(() => {
    let current = true;
    invoke("component_python_request", { componentId: id, versionHash, operation: "status" })
      .then(value => { if (current) setStatus(value); })
      .catch(value => { if (current) setError(String(value)); });
    return () => { current = false; };
  }, [id, versionHash]);

  async function change(operation) {
    setBusy(true);
    setError("");
    try {
      const value = await invoke("component_python_request", { componentId: id, versionHash, operation });
      if (!value.cancelled) setStatus(value);
    } catch (value) {
      setError(String(value));
    } finally {
      setBusy(false);
    }
  }

  const input = status?.input;
  return <details className="stock-watchlist" open>
    <summary>Python 数据插件 <span className="muted small">{status?.enabled ? "已授权" : "待授权"}</span></summary>
    <div className="stock-watchlist__body">
      {input?.kind === "text-file" && <p>允许插件读取你选中的一个文本文件，并将声明的结果字段显示在设备上。{status.file && <>已选择 <strong>{status.file}</strong>。</>}</p>}
      {input?.kind === "https-json" && <p>允许客户端从 <strong>{input.hosts.join("、")}</strong> 读取公开 JSON，并交给插件处理。</p>}
      {input?.kind === "none" && <p>允许插件执行本地计算并返回组件数据。</p>}
      <div className="stock-watchlist__add">
        {status && (!status.enabled || input?.kind === "text-file") && <button type="button" className="btn-primary btn-sm" disabled={busy}
          onClick={() => change("enable")}>{input?.kind === "text-file" ? (status.enabled ? "更换授权文件" : "选择文件并授权") : "授权并启用"}</button>}
        {status?.enabled && <button type="button" className="btn-ghost btn-sm" disabled={busy}
          onClick={() => change("disable")}>撤销授权</button>}
      </div>
      {error && <p className="stock-watchlist__notice" role="alert">{error}</p>}
    </div>
  </details>;
}
