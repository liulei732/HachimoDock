/**
 * [Input] Ordered Agent session snapshots/events and the previous device-visible queue.
 * [Output] Agent-isolated P4 routing sessions, stable 60-second terminal cards, and bounded transport payloads.
 * [Pos] Pure P4 Session service shared by the persistent dashboard orchestrator.
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import { isInternalCodexSession } from "../../src-tauri/bridge/packages/agent-session-bus/src/util/codex-session-filter.mjs";
import {
  DEVICE_SESSION_TERMINAL_HOLD_MS,
  isActiveDeviceSession,
  reconcileDeviceSessionQueue,
} from "./session-display.js";

const P4_DEVICE_SESSION_LIMIT = 8;

// Keep ordering within one owner, but never execute a queued operation after
// its Agent/USB scope has retired. A rejected operation is never replayed.
export function enqueueP4SessionBinding(previous, isCurrent, submit) {
  return previous.catch(() => undefined).then(() => isCurrent() ? submit() : undefined);
}

function retainNewerLifecycle(previous, incoming, merged, concurrentEvent = false) {
  const before = Number(previous?.statusUpdatedAt);
  const after = Number(incoming?.statusUpdatedAt);
  const sameEpoch = previous?.epoch && previous.epoch === incoming?.epoch;
  const seqBefore = Number(previous?.sourceSeq);
  const seqAfter = Number(incoming?.sourceSeq);
  const versionedTie = sameEpoch && before === after && Number.isFinite(seqBefore) && Number.isFinite(seqAfter);
  const older = (before > 0 && after > 0 && after < before) || (versionedTie && seqBefore > seqAfter);
  const ambiguous = !incoming?.state || !after || (after === before && !(versionedTie && seqAfter > seqBefore));
  if (!previous || !(older || (concurrentEvent && ambiguous))) return;
  for (const key of ["state", "statusUpdatedAt", "displayTitle", "displayContent", "status", "active", "activitySource", "stateDetailUnknown", "epoch", "sourceSeq", "transitionRevision"]) {
    if (Object.hasOwn(previous, key)) merged[key] = previous[key];
    else delete merged[key];
  }
}

function normalizeSessionText(value) {
  return typeof value === "string" ? value.trim() : "";
}

const GENERIC_SESSION_TITLES = new Set([
  "会话",
  "Claude 会话",
  "Codex 会话",
  "MiMoCode 会话",
  "WorkBuddy 会话",
  "OpenClaw 会话",
]);

export function p4SessionDisplayTitle(session) {
  const candidates = [session?.name, session?.displayTitle, session?.summary]
    .map(normalizeSessionText)
    .filter((title) => title && !title.startsWith("<recommended_plugins>"));
  return candidates.find((title) => !GENERIC_SESSION_TITLES.has(title))
    || candidates[0]
    || "";
}

export function p4SessionHasMeaningfulTitle(session) {
  const title = p4SessionDisplayTitle(session);
  return Boolean(title && !GENERIC_SESSION_TITLES.has(title));
}

export function buildP4RoutingSessions(sessions) {
  const sessionsById = new Map();
  for (const session of Array.isArray(sessions) ? sessions : []) {
    if (isInternalCodexSession(session)) continue;
    const id = normalizeSessionText(session?.id);
    if (!id) continue;
    const existing = sessionsById.get(id);
    if (!existing || Number(session?.lastModified || 0) > Number(existing?.lastModified || 0)) {
      sessionsById.set(id, session);
    }
  }
  const recency = (session) => Math.max(
    Number(session?.statusUpdatedAt || 0),
    Number(session?.lastModified || 0),
  );
  return [...sessionsById.values()]
    .sort((a, b) => recency(b) - recency(a));
}

export function mergeP4SessionSnapshot(previousSessions, incomingSessions, concurrentIds) {
  const previousById = new Map(
    buildP4RoutingSessions(previousSessions)
      .map((session) => [normalizeSessionText(session?.id), session]),
  );
  const missing = concurrentIds?.size ? new Set(concurrentIds) : null;
  const merged = (Array.isArray(incomingSessions) ? incomingSessions : []).map((session) => {
    const id = normalizeSessionText(session?.id);
    const previous = id ? previousById.get(id) : null;
    if (!previous) return session;
    missing?.delete(id);
    const next = { ...previous, ...session, id };
    const incomingState = normalizeSessionText(session?.state);
    if (!incomingState && normalizeSessionText(previous?.state)) {
      next.state = previous.state;
      next.statusUpdatedAt = Math.max(
        Number(previous?.statusUpdatedAt || 0),
        Number(session?.statusUpdatedAt || 0),
      );
      for (const key of ["displayTitle", "displayContent"]) {
        if (!normalizeSessionText(session?.[key]) && normalizeSessionText(previous?.[key])) {
          next[key] = previous[key];
        }
      }
    }
    for (const key of ["name", "summary", "cwd"]) {
      if (!normalizeSessionText(session?.[key]) && normalizeSessionText(previous?.[key])) {
        next[key] = previous[key];
      }
    }
    next.lastModified = Math.max(
      Number(previous?.lastModified || 0),
      Number(session?.lastModified || 0),
      Number(next.statusUpdatedAt || 0),
    );
    retainNewerLifecycle(previous, session, next, concurrentIds?.has(id));
    return next;
  });
  // A scan already in flight may omit a just-created session. Only preserve
  // rows touched by events during this request, so later scans can prune them.
  if (missing) {
    for (const id of missing) {
      const session = previousById.get(id);
      if (session) merged.push(session);
    }
  }
  return buildP4RoutingSessions(merged);
}

export function mergeP4SessionEvent(previousSessions, incomingSession) {
  const id = normalizeSessionText(incomingSession?.id);
  if (!id) return buildP4RoutingSessions(previousSessions);
  const previous = (Array.isArray(previousSessions) ? previousSessions : [])
    .find((session) => normalizeSessionText(session?.id) === id);
  const statusUpdatedAt = Number(incomingSession?.statusUpdatedAt || 0);
  const merged = {
    ...(previous || {}),
    ...incomingSession,
    id,
    lastModified: Math.max(
      Number(previous?.lastModified || 0),
      Number(incomingSession?.lastModified || 0),
      statusUpdatedAt,
    ),
  };
  retainNewerLifecycle(previous, incomingSession, merged);
  return buildP4RoutingSessions([
    merged,
    ...(Array.isArray(previousSessions) ? previousSessions : [])
      .filter((session) => normalizeSessionText(session?.id) !== id),
  ]);
}

export function buildP4ConversationQueue(
  sessions,
  previousQueue = [],
  nowMs = Date.now(),
  { authoritativeSnapshot = false } = {},
) {
  const eligibleSessions = buildP4RoutingSessions(sessions).filter((session) => (
    !isActiveDeviceSession(session) || p4SessionHasMeaningfulTitle(session)
  ));
  return reconcileDeviceSessionQueue(
    previousQueue,
    eligibleSessions,
    nowMs,
    P4_DEVICE_SESSION_LIMIT,
    { authoritativeSnapshot },
  );
}

export function buildP4DeviceSessionTransportPayload(sessions, nowMs = Date.now()) {
  const numericNow = Number(nowMs);
  const currentTime = Number.isFinite(numericNow) ? numericNow : Date.now();
  return (Array.isArray(sessions) ? sessions : []).map((session) => {
    const { terminalUntilMs, ...payload } = session;
    return {
      ...payload,
      terminalRemainingMs: Math.min(
        DEVICE_SESSION_TERMINAL_HOLD_MS,
        Math.max(0, Math.floor(Number(terminalUntilMs || 0) - currentTime)),
      ),
    };
  });
}

export function p4SessionActivitySignature(session) {
  return JSON.stringify([
    Number(session?.lastModified || 0),
    Number(session?.statusUpdatedAt || 0),
    normalizeSessionText(session?.state),
    session?.active, session?.status, session?.stateDetailUnknown,
    normalizeSessionText(session?.displayContent),
    normalizeSessionText(session?.summary),
  ]);
}

export function filterDismissedP4Sessions(sessions, dismissedSessions = {}) {
  return (Array.isArray(sessions) ? sessions : []).filter((session) => {
    const id = normalizeSessionText(session?.id);
    return !id || dismissedSessions[id] !== p4SessionActivitySignature(session);
  });
}
