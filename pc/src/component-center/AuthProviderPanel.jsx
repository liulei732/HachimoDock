import React, { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";

/* Existing PC authorizations can be reviewed and removed here. */
export default function AuthProviderPanel({ source, request = invoke }) {
  const [status, setStatus] = useState(null);
  const [snapshot, setSnapshot] = useState(null);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const mounted = useRef(false);
  const provider = status?.providers?.find(p => p.source === source);
  async function refresh() {
    try {
      const s = await request("auth_provider_status");
      if (!mounted.current) return;
      setStatus(s);
      const snap = await request("local_widget_snapshot", { source });
      if (mounted.current) setSnapshot(snap);
    } catch (e) { if (mounted.current) setError(String(e)); }
  }
  useEffect(() => { mounted.current = true; refresh(); const timer = setInterval(refresh, 5000); return () => { mounted.current = false; clearInterval(timer); }; }, [source]);
  async function run(name, args = {}) {
    setBusy(true); setError("");
    try { await request(name, args); await refresh(); }
    catch (e) { setError(String(e)); }
    finally { setBusy(false); }
  }
  if (!provider) return <section className="component-app" aria-label="鉴权数据源"><p>{error || "读取提供器状态…"}</p></section>;
  return (
    <section className="component-app" aria-label={`${provider.title}授权`}>
      <div className="component-app__heading"><div><h3>{provider.title}</h3><p>鉴权数据源 · 令牌只保存在这台电脑，不进资源包、设备或日志</p></div><span>{!provider.verified ? "接口待确认" : provider.authorized ? "已授权" : "未授权"}</span></div>
      {!provider.verified && <p role="alert">此数据源的接口契约尚未确认，提供器未启用；组件在设备上显示“接口契约尚未确认”。确认端点、鉴权方式与使用条款后再接入，不会猜测或冒充其他客户端。</p>}
      {provider.verified && <>
        {provider.authorized && <button disabled={busy} onClick={() => { if (window.confirm("删除这台电脑上的授权令牌？设备上的数据会显示未授权。")) run("auth_provider_forget", { source }); }}>移除本机授权</button>}
      </>}
      {snapshot && <div aria-label="数据预览"><p role="status">{snapshot.status === "ok" ? `最近更新 ${snapshot.date}` : snapshot.message}</p>
        {snapshot.rows?.map(r => <div key={r.id}><b>{r.label}</b> {r.value} <small>{r.detail} {r.meta}</small></div>)}</div>}
      {error && <p role="alert">{error}</p>}
    </section>
  );
}
