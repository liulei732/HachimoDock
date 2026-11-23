/**
 * [Input] Followed Agent id, conversation visibility, dismissed-card signatures, and local Agent Bus responses.
 * [Output] Agent-isolated routing Sessions plus the device-visible active/terminal queue.
 * [Pos] Persistent dashboard Session polling and reducer boundary.
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md` and `pc/src/.folder.md`.
 */

import { useEffect, useReducer, useRef } from "react";
import { isActiveDeviceSession, isImplicitTerminalDeviceSession, isTerminalDeviceSession } from "../lib/session-display.js";
import {
  fetchAgentSessionEvents,
  fetchAgentSessions,
} from "../lib/agent-bus-client.js";
import {
  buildP4ConversationQueue,
  filterDismissedP4Sessions,
  mergeP4SessionEvent,
  mergeP4SessionSnapshot,
} from "../lib/p4-session-service.js";

export const P4_SESSION_EVENT_POLL_MS = 750;
export const P4_SESSION_SNAPSHOT_POLL_MS = 5000;
export const P4_SESSION_EVENT_DISPATCH_GAP_MS = 25;
export const P4_PENDING_EVENT_LIMIT = 100;

let lastFeedContact = { agentId: "", at: -Infinity };

export function agentSessionFeedFresh(agentId, maxAgeMs = 9000) {
  return lastFeedContact.agentId === agentId && performance.now() - lastFeedContact.at < maxAgeMs;
}

const EMPTY_SESSIONS = Object.freeze([]);

export const AGENT_SESSION_FEED_INITIAL_STATE = Object.freeze({
  sessions: EMPTY_SESSIONS,
  routingSessions: EMPTY_SESSIONS,
  fingerprint: "",
  loaded: false,
  agentId: "",
});

function normalizeText(value) {
  return typeof value === "string" ? value.trim() : "";
}

function needsPromptDispatch(session) {
  return isTerminalDeviceSession(session) || isImplicitTerminalDeviceSession(session)
    || session.state === "waiting_user";
}

export function queueSessionEvents(pendingEvents, events, apply) {
  for (const event of events) {
    const session = event?.session;
    const id = normalizeText(session?.id);
    if (!id) continue;
    let priorIndex = pendingEvents.length - 1;
    while (priorIndex >= 0 && pendingEvents[priorIndex].session.id !== id) priorIndex--;
    const prior = pendingEvents[priorIndex]?.session;
    // Fold repeated progress reports only. Preserve state changes, chronology,
    // and sparse metadata (for example, a title present in just one event).
    if (!needsPromptDispatch(session) && isActiveDeviceSession(session)
      && prior?.state === session.state && prior.epoch === session.epoch
      && session.statusUpdatedAt >= prior.statusUpdatedAt) {
      pendingEvents[priorIndex] = { ...event, session: { ...prior, ...session } };
    } else {
      // The bridge respects the requested capacity for cursor pages. Its
      // cold/reset bootstrap returns all active rows even above the limit.
      if (pendingEvents.length >= P4_PENDING_EVENT_LIMIT) apply(pendingEvents.shift());
      pendingEvents.push(event);
    }
  }
}

export function dispatchSessionTurn(pendingEvents, apply) {
  const selected = new Set();
  const batchSize = 8;
  for (const { session } of pendingEvents) {
    if (needsPromptDispatch(session)) selected.add(session.id);
    if (selected.size === batchSize) break;
  }
  const urgent = selected.size > 0;
  if (!urgent) {
    for (const { session } of pendingEvents) {
      selected.add(session.id);
      if (selected.size === batchSize) break;
    }
  }
  for (let i = 0; i < pendingEvents.length && selected.size;) {
    const event = pendingEvents[i];
    if (!selected.has(event.session.id)) {
      i++;
      continue;
    }
    pendingEvents.splice(i, 1);
    apply(event);
    // Advance through ordinary prerequisites up to the first urgent transition.
    // A subsequent turn for the same card gets its own render.
    if (!urgent || needsPromptDispatch(event.session)) selected.delete(event.session.id);
  }
}

function nextSessionFeedState(state, action, routingSessions, authoritativeSnapshot) {
  const agentId = action.agentId || "";
  const sameAgent = state.loaded && state.agentId === agentId;
  const displayEnabled = action.displayEnabled !== false;
  const dismissedSessions = action.dismissedSessions || {};
  const previousQueue = displayEnabled && sameAgent
    ? filterDismissedP4Sessions(state.sessions, dismissedSessions)
    : [];
  const visibleRoutingSessions = filterDismissedP4Sessions(
    routingSessions,
    dismissedSessions,
  );
  const sessions = displayEnabled
    ? buildP4ConversationQueue(
      visibleRoutingSessions,
      previousQueue,
      action.nowMs,
      { authoritativeSnapshot },
    )
    : [];
  const fingerprint = JSON.stringify([sessions, routingSessions]);
  if (sameAgent && fingerprint === state.fingerprint) return state;
  return {
    sessions,
    routingSessions,
    fingerprint,
    loaded: true,
    agentId,
  };
}

export function agentSessionFeedReducer(state, action) {
  switch (action.type) {
    case "replace_snapshot": {
      const agentId = action.agentId || "";
      const sameAgent = state.loaded && state.agentId === agentId;
      const previous = sameAgent ? state.routingSessions : [];
      const routingSessions = mergeP4SessionSnapshot(previous, action.value, action.concurrentIds);
      return nextSessionFeedState(state, action, routingSessions, true);
    }
    case "apply_event": {
      const agentId = action.agentId || "";
      const sameAgent = state.loaded && state.agentId === agentId;
      const routingSessions = mergeP4SessionEvent(
        sameAgent ? state.routingSessions : [],
        action.value,
      );
      return nextSessionFeedState(state, action, routingSessions, false);
    }
    case "tick_terminal": {
      const agentId = action.agentId || "";
      if (!state.loaded || state.agentId !== agentId) return state;
      const sessions = action.displayEnabled === false
        ? []
        : buildP4ConversationQueue(
          filterDismissedP4Sessions(state.routingSessions, action.dismissedSessions),
          filterDismissedP4Sessions(state.sessions, action.dismissedSessions),
          action.nowMs,
        );
      const fingerprint = JSON.stringify([sessions, state.routingSessions]);
      if (fingerprint === state.fingerprint) return state;
      return { ...state, sessions, fingerprint };
    }
    case "reset":
      return AGENT_SESSION_FEED_INITIAL_STATE;
    default:
      return state;
  }
}

export function useAgentSessionFeed({ agentId, displayEnabled, dismissedSessions }) {
  const [state, dispatch] = useReducer(
    agentSessionFeedReducer,
    AGENT_SESSION_FEED_INITIAL_STATE,
  );
  const previousAgentIdRef = useRef("");
  const terminalQueuePendingRef = useRef(false);
  const matchesAgent = state.agentId === agentId;
  const sessions = matchesAgent ? state.sessions : EMPTY_SESSIONS;
  const routingSessions = matchesAgent ? state.routingSessions : EMPTY_SESSIONS;
  const loaded = matchesAgent && state.loaded;

  terminalQueuePendingRef.current = sessions.some(isTerminalDeviceSession);

  useEffect(() => {
    if (previousAgentIdRef.current === agentId) return;
    previousAgentIdRef.current = agentId;
    dispatch({ type: "reset" });
  }, [agentId]);

  useEffect(() => {
    if (!agentId) return undefined;

    let cancelled = false;
    let eventCursor;
    let eventStreamId = "";
    let eventsInFlight = false;
    let snapshotInFlight = false;
    let snapshotEvents = null;
    let eventTimer = null;
    let snapshotTimer = null;
    let drainTimer = null;
    let draining = false;
    const pendingEvents = [];
    const ctl = new AbortController();

    const applyEvent = event => {
      snapshotEvents?.add(event.session.id);
      dispatch({
        type: "apply_event",
        agentId,
        value: event.session,
        nowMs: Number(event.updatedAt || event.session.statusUpdatedAt || Date.now()),
        displayEnabled,
        dismissedSessions,
      });
    };
    const drainEventQueue = () => {
      if (cancelled || !pendingEvents.length) {
        draining = false;
        return;
      }
      dispatchSessionTurn(pendingEvents, applyEvent);
      drainTimer = setTimeout(() => {
        drainTimer = null;
        drainEventQueue();
      }, P4_SESSION_EVENT_DISPATCH_GAP_MS);
    };

    const enqueueEvents = (events) => {
      queueSessionEvents(pendingEvents, events, applyEvent);
      if (draining || pendingEvents.length === 0) return;
      draining = true;
      drainEventQueue();
    };

    const pollEvents = async () => {
      if (cancelled || eventsInFlight) return;
      const available = P4_PENDING_EVENT_LIMIT - pendingEvents.length;
      if (!available) return;
      eventsInFlight = true;
      try {
        const result = await fetchAgentSessionEvents(
          agentId,
          eventCursor,
          eventStreamId,
          ctl.signal,
          available,
        );
        if (cancelled) return;
        lastFeedContact = { agentId, at: performance.now() };
        const streamChanged = Boolean(
          eventStreamId && result.streamId && eventStreamId !== result.streamId,
        );
        if (result.reset || streamChanged) {
          pendingEvents.length = 0;
          if (eventStreamId) snapshotEvents = null;
          // The replacement stream bootstraps active Sessions immediately.
          // Keep the last rendered queue until those events arrive so a
          // bridge restart cannot flash every device card off and back on.
        }
        eventCursor = result.cursor;
        eventStreamId = result.streamId;
        enqueueEvents(result.events);
      } catch {
        // Keep the cursor and retry. A successful response resumes or resets it.
      } finally {
        eventsInFlight = false;
        if (!cancelled && terminalQueuePendingRef.current) {
          dispatch({
            type: "tick_terminal",
            agentId,
            nowMs: Date.now(),
            displayEnabled,
            dismissedSessions,
          });
        }
      }
    };

    const refreshSnapshot = async () => {
      if (cancelled || snapshotInFlight) return;
      snapshotInFlight = true;
      const concurrentIds = new Set();
      snapshotEvents = concurrentIds;
      try {
        const nextSessions = await fetchAgentSessions(agentId, ctl.signal);
        if (!cancelled && snapshotEvents === concurrentIds) {
          lastFeedContact = { agentId, at: performance.now() };
          dispatch({
            type: "replace_snapshot",
            agentId,
            value: nextSessions,
            concurrentIds,
            nowMs: Date.now(),
            displayEnabled,
            dismissedSessions,
          });
        }
      } catch {
        // Keep the last confirmed list while event delivery continues.
      } finally {
        snapshotEvents = null;
        snapshotInFlight = false;
      }
    };

    const scheduleEventPoll = () => {
      eventTimer = setTimeout(async () => {
        await pollEvents();
        if (!cancelled) scheduleEventPoll();
      }, P4_SESSION_EVENT_POLL_MS);
    };
    const scheduleSnapshotRefresh = () => {
      snapshotTimer = setTimeout(async () => {
        await refreshSnapshot();
        if (!cancelled) scheduleSnapshotRefresh();
      }, P4_SESSION_SNAPSHOT_POLL_MS);
    };

    void refreshSnapshot().then(() => {
      if (!cancelled) scheduleSnapshotRefresh();
    });
    void pollEvents().then(async () => {
      // Catch up from the initial cursor, independently of metadata scan time.
      await pollEvents();
      if (!cancelled) scheduleEventPoll();
    });

    return () => {
      cancelled = true;
      ctl.abort();
      pendingEvents.length = 0;
      if (eventTimer) clearTimeout(eventTimer);
      if (snapshotTimer) clearTimeout(snapshotTimer);
      if (drainTimer) clearTimeout(drainTimer);
    };
  }, [agentId, dismissedSessions, displayEnabled]);

  return { sessions, routingSessions, loaded };
}
