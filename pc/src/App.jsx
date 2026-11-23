/**
 * [Input] Consume DeviceSetup.jsx and DeviceDashboard.jsx.
 * [Output] Desktop navigation for keyboard configuration, USB setup, authenticated Wi-Fi maintenance, Agent sessions and active component observers. Accessibility consent is requested by native Agent operations.
 * [Pos] component node in pc/src
 * Generation notifications distinguish reference-image rejection with retained partial results from full success.
 * [Sync] If this file changes, update this header and `pc/src/.folder.md`.
 */

import React, { useState, useEffect, useLayoutEffect, useCallback, useRef, lazy, Suspense } from "react";
import { invoke } from "@tauri-apps/api/core";
import {
  ImagePlus,
  Loader2,
  MonitorSmartphone,
  Plus,
  Blocks,
  KeyRound,
  Keyboard,
  Settings,
  Home,
} from "lucide-react";
import DeviceSetup from "./DeviceSetup";
import DeviceDashboard from "./DeviceDashboard";
import KeyboardConfiguration from "./dashboard/KeyboardConfiguration.jsx";
import LanAgentSessionFeed from "./dashboard/LanAgentSessionFeed.jsx";
import DshConnectionPanel, { useDshFollowConnection } from "./dashboard/DshConnectionPanel.jsx";
const AppearanceGallery = lazy(() => import("./AppearanceGallery"));
const CustomAvatarWizard = lazy(() => import("./CustomAvatarWizard"));
const AppearanceDetail = lazy(() => import("./AppearanceDetail"));
const ComponentCenter = lazy(() => import("./ComponentCenter"));
import ComponentCenterBoundary from "./component-center/ComponentCenterBoundary.jsx";
const ApiSettings = lazy(() => import("./ApiSettings"));
const DeviceSettings = lazy(() => import("./DeviceSettings.jsx"));
const SmartHome = lazy(() => import("./SmartHome.jsx"));
import { DeviceContextProvider, useDeviceContext } from "./shell/DeviceContext.jsx";
import ToastStack, { ToastProvider, useToast } from "./shell/ToastStack.jsx";
import FirmwareUpdateNavItem from "./shell/FirmwareUpdateNavItem.jsx";
import {
  acknowledgeGenerationTask,
  subscribeGenerationTask,
} from "./lib/generation-task.js";
import { version as clientVersion } from "../package.json";
import { initializeProviderConfigs } from "./lib/avatar-pipeline/provider-config.js";
import { loadDesktopStartup } from "./lib/desktop-startup.js";
import { installBrowserExitProtection, setExitReason } from "./lib/exit-protection.js";

const DEV_DIRECT_DASHBOARD_BINDING = {
  boardDeviceId: "board-dev-direct-001",
  desktopDeviceId: "desktop-dev-direct-001",
  wifiSsid: "开发直达",
};

function devDirectDashboardBinding() {
  return import.meta.env.DEV && !hasTauriRuntime() ? DEV_DIRECT_DASHBOARD_BINDING : null;
}

function hasTauriRuntime() {
  return typeof window !== "undefined" && Boolean(window.__TAURI_INTERNALS__);
}

export default function App() {
  useEffect(installBrowserExitProtection, []);
  const [view, setView] = useState("loading"); // loading | dashboard | setup | gallery | wizard | detail | components | home | api
  const [binding, setBinding] = useState(null);
  const [detailAppearanceId, setDetailAppearanceId] = useState("");
  const [apiReturnView, setApiReturnView] = useState("");
  const [startupError, setStartupError] = useState("");
  const [configReadError, setConfigReadError] = useState("");
  const [startupAttempt, setStartupAttempt] = useState(0);

  const isPreviewBinding = useCallback((item) => {
    const boardId = String(item?.boardDeviceId || "").trim().toLowerCase();
    return boardId.includes("preview")
      || boardId === "board-ethernet-preview-001"
      || boardId === "board-preview-001";
  }, []);

  const enterBestAvailableDeviceSurface = useCallback((bindings) => {
    const stableBindings = Array.isArray(bindings)
      ? bindings.filter((item) => String(item?.boardDeviceId || "").trim() && !isPreviewBinding(item))
      : [];
    const devBinding = devDirectDashboardBinding();
    if (stableBindings.length > 0) {
      setBinding(stableBindings[stableBindings.length - 1]);
      setView("dashboard");
    } else if (devBinding) {
      setBinding(devBinding);
      setView("dashboard");
    } else {
      setBinding(null);
      setView("setup");
    }
  }, [isPreviewBinding]);

  useEffect(() => {
    let current = true;
    setStartupError("");
    loadDesktopStartup(initializeProviderConfigs, () => invoke("load_device_bindings"))
      .then(({bindings, configError}) => {
        if (!current) return;
        setConfigReadError(configError);
        enterBestAvailableDeviceSurface(bindings);
      })
      .catch(error => {
        if (!current) return;
        if (!hasTauriRuntime()) enterBestAvailableDeviceSurface([]);
        else setStartupError(error?.message || String(error));
      });
    return () => { current = false; };
  }, [enterBestAvailableDeviceSurface, startupAttempt]);

  const handleSetupComplete = useCallback(async () => {
    const bindings = await invoke("load_device_bindings");
    if (!Array.isArray(bindings) || !bindings.some(item =>
      String(item?.boardDeviceId || "").trim() && !isPreviewBinding(item))) {
      throw new Error("设备绑定尚未读回，请重新扫描。");
    }
    enterBestAvailableDeviceSurface(bindings);
  }, [enterBestAvailableDeviceSurface, isPreviewBinding]);

  const handleUnbind = useCallback(() => {
    setBinding(null);
    setView("setup");
  }, []);

  const handleOpenGallery = useCallback(() => {
    setDetailAppearanceId("");
    setView("gallery");
  }, []);

  const handleOpenComponents = useCallback(() => {
    setDetailAppearanceId("");
    setView("components");
  }, []);

  const handleEnterWizard = useCallback(() => setView("wizard"), []);

  const handleWizardExit = useCallback(() => setView("gallery"), []);

  const handleOpenDetail = useCallback((appearanceId) => {
    setDetailAppearanceId(appearanceId);
    setView("detail");
  }, []);

  const handleDetailBack = useCallback(() => {
    setDetailAppearanceId("");
    setView("gallery");
  }, []);

  const handleOpenApiSettings = useCallback(() => {
    if (view === "api") return;
    setApiReturnView(view);
    setView("api");
  }, [view]);

  const handleApiSettingsBack = useCallback(() => {
    setView(apiReturnView && apiReturnView !== "api"
      ? apiReturnView
      : binding
        ? "dashboard"
        : "gallery");
    setApiReturnView("");
  }, [apiReturnView, binding]);

  const galleryViews = new Set(["gallery", "wizard", "detail"]);
  const activeTab = view === "api"
    ? "api"
    : view === "keyboard" ? "keyboard" : view === "settings" ? "settings" : view === "home" ? "home" : view === "components"
      ? "components"
      : galleryViews.has(view)
        ? "gallery"
        : "device";

  if (view === "loading") {
    return (
      <div className="app-shell">
        <div className="auth-shell">
          <div className="auth-card">
            <div className="auth-loading">
              {startupError ? <div role="alert"><p>读取本机配置失败，原数据未清空。</p><p>{startupError}</p>
                <button className="btn-primary" onClick={() => setStartupAttempt(n => n + 1)}>重试</button></div>
                : <><Loader2 size={18} className="spin" /><span>正在打开管理端…</span></>}
            </div>
          </div>
        </div>
      </div>
    );
  }

  return (
    <ToastProvider>
      <DeviceContextProvider
        binding={binding}
        onBindingChange={(next) => setBinding(next)}
      >
        <AppInner
          configReadError={configReadError}
          view={view}
          binding={binding}
          apiReturnView={apiReturnView}
          activeTab={activeTab}
          setView={setView}
          detailAppearanceId={detailAppearanceId}
          handleSetupComplete={handleSetupComplete}
          handleUnbind={handleUnbind}
          handleOpenGallery={handleOpenGallery}
          handleOpenComponents={handleOpenComponents}
          handleOpenApiSettings={handleOpenApiSettings}
          handleApiSettingsBack={handleApiSettingsBack}
          handleEnterWizard={handleEnterWizard}
          handleWizardExit={handleWizardExit}
          handleOpenDetail={handleOpenDetail}
          handleDetailBack={handleDetailBack}
        />
        <ToastStack />
      </DeviceContextProvider>
    </ToastProvider>
  );
}

function AppInner({
  configReadError,
  view,
  binding,
  apiReturnView,
  activeTab,
  setView,
  detailAppearanceId,
  handleSetupComplete,
  handleUnbind,
  handleOpenGallery,
  handleOpenComponents,
  handleOpenApiSettings,
  handleApiSettingsBack,
  handleEnterWizard,
  handleWizardExit,
  handleOpenDetail,
  handleDetailBack,
}) {
  const { push, dismiss } = useToast();
  const contentRef = useRef(null);
  useLayoutEffect(() => {
    if (contentRef.current) contentRef.current.scrollTop = 0;
    if (document.scrollingElement) document.scrollingElement.scrollTop = 0;
  }, [view]);
  const { refresh, usb, wifi, selectedAgentId, activeBoardDeviceId } = useDeviceContext();
  // One persistent owner for both USB dashboard and LAN-only follow paths.
  const dshFollowConnection = useDshFollowConnection(hasTauriRuntime() ? selectedAgentId : "");
  const dashboardBinding = binding || (wifi ? { boardDeviceId: wifi.boardDeviceId } : null);
  const isDashboard = Boolean(dashboardBinding) && (view === "dashboard" || (view === "setup" && !binding));
  const isSetup = (view === "setup" && (Boolean(binding) || !wifi)) || (view === "dashboard" && !dashboardBinding);
  const lastEpochRef = useRef(0);
  const generationToastRef = useRef(null);
  const [componentsVisited, setComponentsVisited] = useState(view === "components");
  useEffect(() => { if (view === "components") setComponentsVisited(true); }, [view]);
  const [settingsVisited, setSettingsVisited] = useState(view === "settings");
  useEffect(() => { if (view === "settings") setSettingsVisited(true); }, [view]);
  const [apiVisited, setApiVisited] = useState(view === "api");
  const [wirelessMaintenanceRequest, setWirelessMaintenanceRequest] = useState(0);
  const openWirelessMaintenance = useCallback(() => {
    setWirelessMaintenanceRequest(request => request + 1);
    setView("settings");
  }, [setView]);
  useEffect(() => { if (view === "api") setApiVisited(true); }, [view]);

  const handleSetupCompleteWithRefresh = useCallback(async () => {
    await refresh();
    await handleSetupComplete();
  }, [handleSetupComplete, refresh]);

  useEffect(() => {
    return subscribeGenerationTask((s) => {
      setExitReason("avatar-generation",s.status === "running" ? "形象生成尚未完成" : "");
      // Closing the gallery result or starting another run also removes its
      // persistent toast. Clear the ref first: dismiss can notify this listener.
      if (s.status !== "completed" && s.status !== "failed") {
        const id = generationToastRef.current;
        generationToastRef.current = null;
        if (id !== null) dismiss(id);
        return;
      }
      if (s.completionEpoch <= lastEpochRef.current) return;
      lastEpochRef.current = s.completionEpoch;
      if (s.status === "completed") {
        refresh().catch((err) => {
          console.warn("[App] refresh after avatar generation failed", err);
        });
      }
      generationToastRef.current = push({
        tone: s.status === "completed" ? "success" : "error",
        title:
          s.status === "completed"
            ? `「${s.appearanceName}」${s.progress?.referenceImageError || s.resumable ? "部分生成完成" : "生成完成"}`
            : `「${s.appearanceName}」${s.appearanceId ? "部分已保存，后续未完成" : "生成失败"}`,
        message: s.status === "failed" ? s.error : s.progress?.referenceImageError ? s.progress.message : "",
        ttl: 0,
        onDismiss: () => acknowledgeGenerationTask(s.completionEpoch),
        action: s.appearanceId
          ? {
              label: "查看",
              onClick: () => {
                acknowledgeGenerationTask(s.completionEpoch);
                handleOpenDetail(s.appearanceId);
              },
            }
          : null,
      });
    });
  }, [push, dismiss, handleOpenDetail, refresh]);

  return (
    <div className="app-shell">
      <div className="app-layout">
        <aside className="app-sidebar">
          <div className="sidebar-head">
            <div className="sidebar-brand">
              <span className="sidebar-brand__copy">
                <strong className="sidebar-brand-name">HachimoDock</strong>
                <span>桌宠管理端 · v{clientVersion}</span>
              </span>
            </div>
          </div>
          <nav className="sidebar-nav" aria-label="主导航">
            <button
              type="button"
              className={`sidebar-nav__item ${activeTab === "device" ? "is-active" : ""}`} aria-current={activeTab === "device" ? "page" : undefined}
              onClick={() => setView(dashboardBinding ? "dashboard" : "setup")}
              title="桌宠伙伴"
            >
              <MonitorSmartphone size={16} />
              <span className="sidebar-nav-label">桌宠伙伴</span>
            </button>
            <button
              type="button"
              className={`sidebar-nav__item ${activeTab === "gallery" ? "is-active" : ""}`} aria-current={activeTab === "gallery" ? "page" : undefined}
              onClick={handleOpenGallery}
              title="形象画廊"
            >
              <ImagePlus size={16} />
              <span className="sidebar-nav-label">形象画廊</span>
            </button>
            <button
              type="button"
              className={`sidebar-nav__item ${activeTab === "components" ? "is-active" : ""}`} aria-current={activeTab === "components" ? "page" : undefined}
              onClick={handleOpenComponents}
              title="组件中心"
            >
              <Blocks size={16} />
              <span className="sidebar-nav-label">组件中心</span>
            </button>
            <button type="button" className={`sidebar-nav__item ${activeTab === "home" ? "is-active" : ""}`} aria-current={activeTab === "home" ? "page" : undefined} onClick={() => setView("home")} title="智能家居">
              <Home size={16} /><span className="sidebar-nav-label">智能家居</span>
            </button>
            <button type="button" className={`sidebar-nav__item ${activeTab === "keyboard" ? "is-active" : ""}`} aria-current={activeTab === "keyboard" ? "page" : undefined} onClick={() => setView("keyboard")} title="键盘配置">
              <Keyboard size={16} /><span className="sidebar-nav-label">键盘配置</span>
            </button>
            <button
              type="button"
              className={`sidebar-nav__item ${activeTab === "api" ? "is-active" : ""}`} aria-current={activeTab === "api" ? "page" : undefined}
              onClick={handleOpenApiSettings}
              title="模型配置"
            >
              <KeyRound size={16} />
              <span className="sidebar-nav-label">模型配置</span>
            </button>
            <button type="button" className={`sidebar-nav__item ${activeTab === "settings" ? "is-active" : ""}`} aria-current={activeTab === "settings" ? "page" : undefined} onClick={() => setView("settings")} title="其它设置">
              <Settings size={16} /><span className="sidebar-nav-label">其它设置</span>
            </button>
            <FirmwareUpdateNavItem usb={usb} wifi={wifi} />
          </nav>
          {!activeBoardDeviceId && (
            <button type="button" className="sidebar-bind" onClick={() => setView("setup")}>
              <Plus size={14} />绑定设备
            </button>
          )}
        </aside>
        <section className="app-main">
          <main className="app-content" ref={contentRef}>
            {configReadError && <div className="message-banner message-banner--warning" role="alert">
              <strong>已有 API 配置暂时无法读取，原文件已保留。</strong>
              <p>设备管理和本地组件仍可使用；依赖这些凭据的服务暂不可用。不会自动清空或覆盖原配置。</p>
              <p>{configReadError}</p>
            </div>}
            {hasTauriRuntime() && selectedAgentId === "deepseek-harness" && (isDashboard || isSetup)
              && <DshConnectionPanel connection={dshFollowConnection} />}
            <Suspense fallback={<p role="status">正在载入页面…</p>}>
            {isSetup && <>
              <DeviceSetup onComplete={handleSetupCompleteWithRefresh} onOpenPairing={() => setView("settings")} />
            </>}
            {!dashboardBinding && hasTauriRuntime() && (
              <LanAgentSessionFeed agentId={selectedAgentId} usbConnected={usb.connected} />
            )}
            {!dashboardBinding && view === "keyboard" && <KeyboardConfiguration usb={usb} />}
            {dashboardBinding && (
              <DeviceDashboard
                onOpenDeviceSettings={() => setView("settings")}
                active={isDashboard}
                keyboardActive={view === "keyboard"}
                binding={dashboardBinding}
                onSwitchToSetup={() => setView("setup")}
                onUnbind={binding && !wifi ? handleUnbind : undefined}
                onOpenGallery={handleOpenGallery}
                onOpenDetail={handleOpenDetail}
                onOpenApiSettings={handleOpenApiSettings}
              />
            )}
            {(view === "gallery" || (view === "api" && apiReturnView === "gallery")) && (
              <div hidden={view !== "gallery"}>
              <AppearanceGallery
                binding={binding}
                onEnterWizard={handleEnterWizard}
                onOpenDetail={handleOpenDetail}
                onOpenApiSettings={handleOpenApiSettings}
              />
              </div>
            )}
            {(view === "wizard" || (view === "api" && apiReturnView === "wizard")) && (
              <div hidden={view !== "wizard"}>
                <CustomAvatarWizard
                  onExit={handleWizardExit}
                  onOpenApiSettings={handleOpenApiSettings}
                />
              </div>
            )}
            {(view === "detail" || (view === "api" && apiReturnView === "detail"))
              && detailAppearanceId && (
                <div hidden={view !== "detail"}>
                  <AppearanceDetail
                    appearanceId={detailAppearanceId}
                    boardDeviceId={usb.connected && (!binding?.boardDeviceId || binding.boardDeviceId === usb.boardDeviceId)
                      ? usb.boardDeviceId : ""}
                    wifiConnected={Boolean(wifi)}
                    onBack={handleDetailBack}
                    onOpenApiSettings={handleOpenApiSettings}
                  />
                </div>
            )}
            {(componentsVisited || view === "components") && (
              <div hidden={view !== "components"}>
                <ComponentCenterBoundary><ComponentCenter active={view === "components"} onOpenWirelessMaintenance={openWirelessMaintenance} /></ComponentCenterBoundary>
              </div>
            )}
            {(apiVisited || view === "api") && (
              <div hidden={view !== "api"}>
                <ApiSettings active={view === "api"} onBack={handleApiSettingsBack} configReadError={configReadError} />
              </div>
            )}
            {view === "home" && <SmartHome />}
            {(settingsVisited || view === "settings") && <div hidden={view !== "settings"}><DeviceSettings usb={usb} wifi={wifi} active={view === "settings"}
              wirelessMaintenanceRequest={wirelessMaintenanceRequest} /></div>}
            </Suspense>
          </main>
        </section>
      </div>
    </div>
  );
}
