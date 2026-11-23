"use strict";

function preview(text, limit) {
  if (typeof text !== "string") return "";
  let result = "";
  let bytes = 0;
  for (const char of text.replace(/\s+/g, " ").trim()) {
    bytes += Buffer.byteLength(char);
    if (bytes > limit) break;
    result += char;
  }
  return result;
}

const tools = new Map([
  ["run_command", "运行命令"],
  ["view_file", "读取文件"],
  ["write_to_file", "写入文件"],
  ["replace_file_content", "编辑文件"],
  ["multi_replace_file_content", "编辑文件"],
  ["grep_search", "搜索文件"],
]);

// Only the user-facing title and response leave the parser. Tool arguments,
// raw tool results, and private reasoning are used neither as text nor metadata.
function parseAntigravityStep(line, previous = {}) {
  let row;
  try {
    row = JSON.parse(line);
  } catch {
    return null;
  }
  if (!row || typeof row !== "object") return null;

  const title = previous.title || "Antigravity";
  const phase = (state, event, content) => ({ state, event: `antigravity:${event}`, title, content });
  if (row.status === "ERROR") return phase("error", "error", "执行失败");

  switch (row.type) {
    case "USER_INPUT": {
      const text = typeof row.content === "string" ? row.content : "";
      const request = text.match(/<USER_REQUEST>([\s\S]*?)<\/USER_REQUEST>/)?.[1];
      return {
        ...phase("working", "user_input", "正在思考"),
        title: preview(request ?? text, 84) || title,
      };
    }
    case "PLANNER_RESPONSE": {
      if (Array.isArray(row.tool_calls) && row.tool_calls.length) {
        return phase("working", "tool_call", tools.get(row.tool_calls[0]?.name) || "正在使用工具");
      }
      const response = preview(row.content, 383);
      if (response) {
        return row.status === "DONE" || !row.status
          ? phase("done", "response", response)
          : phase("speaking", "response", response);
      }
      if (row.status === "DONE" && previous.state === "speaking") {
        return phase("done", "completed", previous.content);
      }
      if ((typeof row.thinking === "string" && row.thinking.trim()) || (row.status && row.status !== "DONE")) {
        return phase("working", "thinking", "正在思考");
      }
      return phase("done", "completed", "已完成");
    }
    case "GENERIC":
      return previous.state === "working" ? phase("working", "tool_result", "正在处理工具结果") : null;
    default:
      return null;
  }
}

module.exports = { parseAntigravityStep };
