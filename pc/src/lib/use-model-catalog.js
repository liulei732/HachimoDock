import { useCallback, useMemo, useState } from "react";

// A manual refresh applies to the exact provider, endpoint and key at click time.
export function useModelCatalog(autoLoad, provider, baseUrl, apiKey) {
  const [request, setRequest] = useState(null);
  const scope = useMemo(() => ({}), [provider, baseUrl, apiKey]);
  const refresh = useCallback(() => {
    setRequest(previous => ({ scope, revision: (previous?.revision || 0) + 1 }));
  }, [scope]);
  const requested = autoLoad || request?.scope === scope;
  return { requested, revision: request?.revision || 0, refresh };
}
