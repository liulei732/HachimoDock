import React from "react";
import { createRoot } from "react-dom/client";
import Modal from "../../src/PersonaVoiceModal.jsx";
import Gallery from "../../src/AppearanceGallery.jsx";
import { DeviceContext } from "../../src/shell/DeviceContext.jsx";
import { ToastProvider } from "../../src/shell/ToastStack.jsx";
import { normalizePersonaVoice } from "../../src/lib/persona-voice.js";
import { API_CONFIGURATION_UPDATED_EVENT } from "../../src/lib/api-configuration.js";

export function mount() {
  const root = createRoot(document.getElementById("root"));
  window.changeRoute = (updates, event = API_CONFIGURATION_UPDATED_EVENT) => {
    Object.assign(window.statusFixture, updates);
    window.dispatchEvent(new Event(event));
  };
  window.openEditor = (key, previewAvailable) => {
    const appearance = { id: "builtin-terrier", name: "测试宠物",
      personaVoice: normalizePersonaVoice(window.documentFixture, { isBuiltin: true }) };
    root.render(<Modal key={key} appearance={appearance} previewAvailable={previewAvailable}
      onClose={() => root.render(null)} onSaved={() => root.render(null)} />);
  };
  window.openGallery = () => root.render(
    <DeviceContext.Provider value={{ currentDisplay: {}, deviceConnected: false }}>
      <ToastProvider><Gallery /></ToastProvider>
    </DeviceContext.Provider>
  );
  window.openEditor(1, true);
}
