// Single-flight fallback reads must never overwrite a newer push/start/stop event.
export function createStatusReconciler() {
  let revision = 0;
  let pending;
  let disposed = false;
  return {
    invalidate({ replaceTarget = false } = {}) {
      revision += 1;
      if (replaceTarget) pending = null;
    },
    dispose() { disposed = true; revision += 1; },
    poll(read, apply) {
      if (disposed) return Promise.resolve(null);
      if (pending) return pending;
      const startedAt = revision;
      const request = (async () => {
        const status = await read();
        if (disposed || startedAt !== revision) return null;
        if (status) apply(status);
        return status;
      })().finally(() => {
        if (pending === request) pending = null;
      });
      pending = request;
      return request;
    },
  };
}

export function mergeStatus(current, status) {
  if (current == null) return { ...status };
  for (const [key, value] of Object.entries(status ?? {})) {
    const old = current[key];
    if (Object.is(old, value)) continue;
    if (old && value && typeof old === "object" && typeof value === "object" &&
        JSON.stringify(old) === JSON.stringify(value)) continue;
    return { ...current, ...status };
  }
  return current;
}
