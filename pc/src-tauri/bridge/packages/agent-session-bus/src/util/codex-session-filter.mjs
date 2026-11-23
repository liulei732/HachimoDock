// This classifier is shared by desktop routing and the bundled Node bridge.
const internalOrigins = new Set([
  "guardian-review", "codex-auto-review", "approvals-reviewer", "auto-review", "approval-review",
]);
const reviewFields = ["risk_level", "user_authorization", "outcome", "rationale"];
const reviewTitle = /^the following is the code(?: under review)?[.:]?$/i;
const legacyPrompt = /^the following is the code(?: under review)?[.:]\s+return risk_level,\s*user_authorization,\s*outcome,?\s+and rationale\.?$/i;

export function isInternalCodexReviewText(value) {
  return typeof value === "string" && legacyPrompt.test(value.trim());
}

function isReviewResult(value) {
  if (typeof value !== "string" || !value.trimStart().startsWith("{")) return false;
  try {
    const result = JSON.parse(value);
    return reviewFields.every(key => typeof result?.[key] === "string");
  } catch {
    return false;
  }
}

export function isInternalCodexSession(session) {
  if (!session || typeof session !== "object") return false;
  const origins = [session.threadSource, session.thread_source, session.model, session.originator];
  if (origins.some(value => typeof value === "string" &&
    internalOrigins.has(value.trim().toLowerCase().replace(/[_ ]/g, "-")))) return true;
  // The P4 list also serves other agents. Never apply Codex prompt heuristics
  // to a session whose adapter explicitly identifies another agent.
  if (typeof session.agentId === "string" && session.agentId.trim()
      && session.agentId.trim().toLowerCase() !== "codex") return false;
  const titles = [session.title, session.name, session.sessionTitle, session.displayTitle, session.summary];
  const bodies = [session.displayContent, session.firstUserMessage, session.lastUserMessage, session.lastAgentMessage];
  if ([...titles, ...bodies].some(isInternalCodexReviewText)) return true;
  return titles.some(value => typeof value === "string" && reviewTitle.test(value.trim()))
    && bodies.some(isReviewResult);
}
