import React, { useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { useDeviceContext } from "../shell/DeviceContext.jsx";
import { useToast } from "../shell/ToastStack.jsx";

const editions = {
  "book-pocket": "导入离线书袋",
  "hachimi-learn": "导入离线知识精选",
  "cozy-pop-stars": "导入每日与续局版",
  "tomato-clock": "导入当前宠物专注版",
};

async function localPortrait(appearance) {
  const src = appearance?.source_image_src;
  if (!src) return null;
  const url = new URL(src, window.location.href);
  if (!(url.origin === window.location.origin || url.protocol === "asset:"
      || (["http:", "https:"].includes(url.protocol) && url.hostname === "asset.localhost"))) return null;
  const image = new Image();
  image.crossOrigin = "anonymous";
  image.src = url.href;
  try {
    await image.decode();
    const canvas = document.createElement("canvas");
    canvas.width = canvas.height = 96;
    const ctx = canvas.getContext("2d");
    const side = Math.min(image.naturalWidth, image.naturalHeight);
    ctx.drawImage(image, (image.naturalWidth-side)/2, (image.naturalHeight-side)/2, side, side, 0, 0, 96, 96);
    return Array.from(ctx.getImageData(0, 0, 96, 96).data);
  } catch {
    return null; // The named greeting also works for a video-only appearance.
  }
}

export default function OfflineEditionButton({ componentId, disabled, onImported }) {
  const { currentDisplay } = useDeviceContext();
  const { push } = useToast();
  const [busy, setBusy] = useState(false);
  if (!Object.hasOwn(editions, componentId)) return null;
  const focus = componentId === "tomato-clock";
  const pet = currentDisplay?.appearance;
  async function importEdition() {
    setBusy(true);
    try {
      const entry = await invoke("import_component_edition", {
        component: componentId, petName: focus ? pet?.name : null,
        portrait: focus ? await localPortrait(pet) : null,
      });
      await onImported?.();
      push({ tone: "success", title: `已导入「${entry.name}」`, message: "可在组件库中预览并保存到设备。" });
    } catch (error) {
      push({ tone: "error", title: "导入实验版失败", message: String(error) });
    } finally {
      setBusy(false);
    }
  }
  return <button type="button" className="btn-ghost btn-sm"
    disabled={disabled || busy || (focus && (!pet?.name || currentDisplay?.appearanceUnverified))}
    onClick={importEdition}>{busy ? "正在导入…" : editions[componentId]}</button>;
}
