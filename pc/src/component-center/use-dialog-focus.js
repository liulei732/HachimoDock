import { useEffect, useRef } from "react";

const focusable = 'button:not(:disabled), input:not(:disabled), select:not(:disabled), textarea:not(:disabled), a[href], summary, [tabindex="0"]';

function controls(dialog) {
  return Array.from(dialog.querySelectorAll(focusable)).filter(node => node.getClientRects().length);
}

export function useDialogFocus(onClose, busy = false, open = true) {
  const ref = useRef(null);
  const latest = useRef({ onClose, busy });
  latest.current = { onClose, busy };

  useEffect(() => {
    const dialog = ref.current;
    if (!open || !dialog) return;
    const previous = document.activeElement;
    const first = () => controls(dialog)[0] || dialog;
    if (!dialog.contains(previous)) first().focus();

    function handleKey(event) {
      const top = Array.from(document.querySelectorAll('[aria-modal="true"]')).filter(el => el.getClientRects().length).at(-1);
      if (!dialog.getClientRects().length || (top && top !== dialog)) return;
      if (event.key === "Escape") {
        event.preventDefault();
        event.stopPropagation();
        if (!latest.current.busy) latest.current.onClose?.();
      } else if (event.key === "Tab") {
        const items = controls(dialog);
        const edge = event.shiftKey ? items[0] : items.at(-1);
        if (!items.length || !dialog.contains(document.activeElement) || document.activeElement === edge) {
          event.preventDefault();
          (event.shiftKey ? items.at(-1) || dialog : first()).focus();
        }
      }
    }
    document.addEventListener("keydown", handleKey, true);
    return () => {
      document.removeEventListener("keydown", handleKey, true);
      if (previous?.isConnected && previous.getClientRects().length) previous.focus({ preventScroll: true });
    };
  }, [open]);

  useEffect(() => {
    const dialog = ref.current;
    if (open && !busy && dialog?.getClientRects().length && !dialog.contains(document.activeElement)) {
      (controls(dialog)[0] || dialog).focus();
    }
  }, [busy, open]);
  return ref;
}
