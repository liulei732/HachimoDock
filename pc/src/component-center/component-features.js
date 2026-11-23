import {featureView} from '../../../server/community/web/feature-view.mjs';
export {featureView};

import {wifiComponentTarget} from "../lib/wifi-device-control.js";

// Never borrow a wireless observation from another USB board or a previous boot.
export function featureTarget(usb, wifi) {
  const trusted = wifi?.controlTrusted === true && wifi?.wifiConnected === true;
  if (usb?.connected && usb.boardDeviceId) {
    const boot = usb.bootId ?? usb.runtime?.bootId;
    const sameBoot = trusted && wifi.boardDeviceId === usb.boardDeviceId && boot && wifi.bootId === boot;
    return {capabilities:usb.capabilities ?? {},wifiConnected:sameBoot ? true : usb.wifiConnected};
  }
  return trusted ? {capabilities:wifiComponentTarget(wifi)?.capabilities ?? {},wifiConnected:true} : null;
}

export function componentFeatures(item,target) {
  return featureView(item?.valid === false || item?.isDeviceOnly ? null : item?.featureManifest,target);
}
