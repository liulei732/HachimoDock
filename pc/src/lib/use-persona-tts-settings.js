import { useEffect, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { API_CONFIGURATION_UPDATED_EVENT } from "./api-configuration.js";
import { formatModelIssue } from "./model-issue.js";

export function usePersonaTtsSettings() {
  const [snapshot, setSnapshot] = useState({ loading: true, status: null, error: "" });
  useEffect(() => {
    let revision = 0;
    async function refresh() {
      const request = ++revision;
      setSnapshot(current => ({ ...current, loading: true }));
      try {
        const { ttsConfigured, ttsProvider, ttsModel, ttsBaseUrl, ttsVoice } = await invoke("load_voice_chat_settings");
        if (revision === request) setSnapshot({
          loading: false, error: "", status: { ttsConfigured, ttsProvider, ttsModel, ttsBaseUrl, ttsVoice },
        });
      } catch (error) {
        if (revision === request) setSnapshot({ loading: false, status: null, error: formatModelIssue("storage", "本机试听配置", error) });
      }
    }
    refresh();
    window.addEventListener(API_CONFIGURATION_UPDATED_EVENT, refresh);
    window.addEventListener("focus", refresh);
    return () => {
      revision++;
      window.removeEventListener(API_CONFIGURATION_UPDATED_EVENT, refresh);
      window.removeEventListener("focus", refresh);
    };
  }, []);
  return snapshot;
}
