import React from "react";
import { createRoot } from "react-dom/client";
import ApiSettings from "../src/ApiSettings.jsx";
import BoardButtonPanel from "../src/dashboard/BoardButtonPanel.jsx";
import { DEFAULT_VOICE_CONFIG } from "../src/DeviceDashboard.jsx";
import { initializeProviderConfigs } from "../src/lib/avatar-pipeline/provider-config.js";
import "../src/styles.css";
import "../src/companion-theme.css";

export async function mountSettings() {
  await initializeProviderConfigs();
  const root = createRoot(document.getElementById("root"));
  root.render(<ApiSettings />);
  return () => root.render(<BoardButtonPanel runtime="esp-p4" voiceConfig={DEFAULT_VOICE_CONFIG} usbConnected />);
}
