import { useCallback, useEffect, useLayoutEffect, useRef } from "react";
import { invoke } from "@tauri-apps/api/core";
import { appearanceTarget, deliverAppearance } from "./appearance-delivery.js";

const useTargetEffect = typeof window === "undefined" ? useEffect : useLayoutEffect;

export function useAppearanceDelivery(usb, wifi, boundBoard, appearanceId) {
  const target = appearanceTarget(usb, wifi, boundBoard);
  const scope = JSON.stringify([target, appearanceId]);
  const current = useRef(null);
  const pending = useRef(null);
  useTargetEffect(() => {
    const owner = { target, appearanceId };
    current.current = owner;
    return () => { current.current = null; };
  }, [scope]);

  const send = useCallback(async () => {
    const owner = current.current;
    if (!owner?.target || pending.current) return null;
    const requestId = crypto.randomUUID();
    pending.current = requestId;
    try {
      const message = await deliverAppearance(invoke, owner.target, owner.appearanceId, requestId);
      return current.current === owner ? message : null;
    } catch (error) {
      if (current.current === owner) throw error;
      return null;
    } finally {
      pending.current = null;
    }
  }, []);
  const cancel = useCallback(() => pending.current
    ? invoke("usb_cancel_appearance_sync", { requestId: pending.current })
    : Promise.resolve({ requested: false }), []);
  return { available: !!target && !!appearanceId, send, cancel };
}
