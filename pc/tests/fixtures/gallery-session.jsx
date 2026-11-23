import React, { useLayoutEffect } from "react";
import { createRoot } from "react-dom/client";
import AppearanceGallery from "../../src/AppearanceGallery.jsx";
import { ToastProvider } from "../../src/shell/ToastStack.jsx";
import { useAgentSessionFeed } from "../../src/dashboard/useAgentSessionFeed.js";
import { saveGenerationSession } from "../../src/lib/avatar-pipeline/session.js";
import { markOnboardingSeen } from "../../src/lib/onboarding-state.js";

const root = createRoot(document.getElementById("root"));
const dismissedSessions = {};
const events = {};
window.bStates = [];
const calls = window.calls = [];
const image = Uint8Array.from(atob("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jB5sAAAAASUVORK5CYII="), c => c.charCodeAt(0));
window.__TAURI_INTERNALS__ = {
  invoke: async (command, args) => {
    calls.push({command, args});
    if (command === "load_voice_chat_settings") return {};
    if (command === "plugin:fs|read_file") return image;
    if (command === "local_bridge_request") {
      const route = new URL(args.route, location.origin);
      const id = route.searchParams.get("agentId");
      if (route.pathname.endsWith("/session-events")) {
        if (id === "C" && !route.searchParams.has("cursor")) {
          return { streamId:id, cursor:1, events:[
            {session:{id:"live-C",name:"C task",state:"working",statusUpdatedAt:10}},
          ] };
        }
        return new Promise(resolve => { events[id] = resolve; });
      }
      if (id === "C") {
        return new Promise(resolve => { window.releaseSnapshot = () => resolve({sessions:[]}); });
      }
      await new Promise(resolve => setTimeout(resolve, 100));
      return {sessions:[{id, name:id, state:"working", statusUpdatedAt:1}]};
    }
    throw Error("Unexpected IPC: " + command);
  },
};

function Feed({id}) {
  const feed = useAgentSessionFeed({agentId:id,displayEnabled:true,dismissedSessions});
  useLayoutEffect(() => {
    if (id === "B" && feed.loaded && !window.firstB) window.firstB = performance.now();
    const card = feed.sessions.find(s => s.id === "B");
    if (card && window.bStates.at(-1)?.state !== card.state) {
      window.bStates.push({state:card.state, time:performance.now()});
    }
  }, [id, feed]);
  return <output>{feed.routingSessions.map(s => s.id).join(",")}</output>;
}
window.showFeed = id => {
  window.started = performance.now();
  root.render(<Feed id={id}/>);
};
window.releaseEvents = id => {
  const now = Date.now();
  window.burstStart = performance.now();
  events[id]({streamId:id, cursor:100, events:[
    ...Array.from({length:97}, (_, i) => ({session:{id:"other-"+i,state:"thinking",statusUpdatedAt:now}})),
    {session:{id,state:"done",statusUpdatedAt:now}},
    {session:{id,state:"working",statusUpdatedAt:now+1}},
    {session:{id:id+"-late",name:"late",state:"working",statusUpdatedAt:now+2}},
  ]});
};

window.showGallery = async () => {
  markOnboardingSeen("appearance-gallery");
  await saveGenerationSession({id:"saved", appearanceName:"已存试片", actions:{welcome:{videoBytes:image}}});
  const get = IDBObjectStore.prototype.get;
  IDBObjectStore.prototype.get = function(key) {
    const request = get.call(this, key);
    if (this.name === "jobs" && key === "current") {
      this.transaction.abort();
      IDBObjectStore.prototype.get = get;
    }
    return request;
  };
  root.render(<ToastProvider><AppearanceGallery /></ToastProvider>);
};
