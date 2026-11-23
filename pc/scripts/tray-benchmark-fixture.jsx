import React from "react";
import {createRoot} from "react-dom/client";
import {DeviceContextProvider} from "../src/shell/DeviceContext.jsx";
import {ToastProvider} from "../src/shell/ToastStack.jsx";
import Dashboard from "../src/DeviceDashboard.jsx";
import SmartHome from "../src/SmartHome.jsx";

// App.jsx keeps the bound Dashboard mounted when home or keyboard settings are open.
export function mount(scenario, boardDeviceId) {
  const binding={boardDeviceId};
  return createRoot(document.querySelector("#root")).render(
    <ToastProvider><DeviceContextProvider binding={binding}>
      <Dashboard binding={binding} active={scenario==="usb-dashboard"} keyboardActive={scenario==="usb-keyboard"} />
      {scenario==="unplugged-smart-home" && <SmartHome/>}
    </DeviceContextProvider></ToastProvider>
  );
}
