import { formatModelIssue } from "./model-issue.js";
import { formatDiagnostic, transportHint } from "./avatar-pipeline/diagnostics.js";
import { getArkImageEditPolicyRejection } from "./avatar-pipeline/providers/volcano-image.js";
import { getVolcanoInputImageRejection } from "./avatar-pipeline/providers/volcano.js";

// Recognize request-site labels, including records saved by older clients.
// A video workflow also edits images, analyzes prompts and writes local files.
function requestContext(raw, diagnosticStage, videoProvider) {
  const label = raw.replace(/^参考图处理失败[^\n]*?(?=Ark image edit)/, "");
  if (/^Ark image edit\b/.test(label) || diagnosticStage === "image_edit") return ["image", "火山方舟", "图片处理"];
  if (/^(?:参考图检查|试片检查)\s*\(/.test(label) || ["reference_review", "trial_review"].includes(diagnosticStage))
    return ["chat", "", "自动检查"];
  if (/^thinking model\b/i.test(label) || diagnosticStage === "thinking") return ["chat", "", "图像理解 / 提示词"];
  if (/^(?:volcano (?:submit|query|task)|火山引擎模型(?:未开通|不可用))/i.test(label)) return ["video", "火山方舟", "视频生成"];
  if (/^(?:可灵 AI|kling (?:submit|query|task))/i.test(label)) return ["video", "可灵", "视频生成"];
  if (/^custom (?:submit|query|task|provider)\b/i.test(label)) return ["video", "自定义", "视频生成"];
  if (["video_submit", "video_poll"].includes(diagnosticStage)) return ["video", videoProvider || "", "视频生成"];
  if (diagnosticStage === "download") return ["network", "", "下载生成素材"];
  return ["", "", "形象生成"];
}

export function normalizeGenerationErrorMessage(error, videoProvider) {
  const raw = String(error?.message || error || "").trim();
  if (!raw) return "生成失败：未收到具体错误信息。";
  if (raw.startsWith("生成失败：所有动作都没有生成成功。")) return raw.slice(0, 1400);
  if (error?.code === "QUALITY_OUTPUT_INVALID" || error?.code === "PROMPT_OUTPUT_INVALID") {
    // The local parser owns this recovery message. A 200 response with invalid
    // output must not be presented as a network or access failure.
    return raw;
  }
  const diagnostic = error?.diagnostic || error?.cause?.diagnostic;
  const [stage, labelProvider, operation] = requestContext(raw, error?.requestStage || error?.cause?.requestStage || diagnostic?.stage, videoProvider);
  const provider = error?.requestProvider || error?.cause?.requestProvider || labelProvider;
  const primary = raw.split(/\n(?:Volcano payload summary:|诊断摘要：|原始错误：)/)[0].trim();
  const imageRejection = getArkImageEditPolicyRejection(raw);
  if (imageRejection) return imageRejection.message;
  if ((stage === "video" && labelProvider === "火山方舟") || /^图生视频失败：/.test(primary)) {
    const rejection = getVolcanoInputImageRejection(raw);
    if (rejection) return rejection.message;
  }
  // Already localized family failure from an earlier boundary.
  if (/^(?:图片处理|图像理解 \/ 提示词|自动检查|视频生成|形象生成)模型(?:未开通或无权限|不可用)：/.test(primary) ||
      /^(?:火山方舟|可灵|自定义) · /.test(primary)) return primary;
  if (/模型未开通|模型不可用|ModelNotOpen|InvalidEndpointOrModel\.NotFound|has not activated the model/i.test(primary)) {
    const model = /^(?:Ark image edit|thinking model|参考图检查|试片检查)\s*\(([a-z0-9._-]{1,128})\)/i.exec(primary)?.[1]
      || /^火山引擎模型(?:未开通|不可用)：(?:当前 Ark 账号还没有开通 )?([a-z0-9._-]{1,128})/i.exec(primary)?.[1];
    const reason = /InvalidEndpointOrModel\.NotFound|模型不可用/i.test(primary) ? "模型不可用" : "模型未开通或无权限";
    return `${operation}${reason}：${model && !model.startsWith("sk-") ? model : "模型 ID 未获取"}。请检查模型 ID、开通状态和当前 Key 的权限。`;
  }
  if (stage === "video" && provider === "火山方舟" && /expected the height to be at least 300px/i.test(primary))
    return "参考图尺寸不满足火山引擎要求：图片高度至少需要 300px。请上传更高分辨率的参考图，或启用 400x300 快速参考帧处理。";
  if (stage === "chat" && /do not support image input/i.test(primary))
    return "文字分析模型不支持图片输入：请选择支持图像理解的多模态模型，再生成提示词。";
  const http = /\bHTTP(?:\/\d(?:\.\d)?)?\s*[:：]?\s*([45]\d\d)\b/i.exec(primary);
  // Keep local instructions about an already accepted task even if polling or
  // result download failed. These paths must never recommend paying again.
  const task = /^(?:(?:可灵 AI 任务|volcano task) [A-Za-z0-9_-]{1,100} (?:查询超时|timed out)|custom provider: timed out .* waiting for task)/i.test(primary);
  if (task) return formatModelIssue("video", provider, "timeout") + " 已提交视频任务，请在服务控制台核对现有任务，避免重复提交。";
  if (/^可灵 AI (?:提交响应缺少任务 ID|查询响应缺少对应任务|任务成功，但未返回所选视频的下载地址)/.test(primary))
    return primary;
  if (stage || http || /^(?:Failed to fetch|NetworkError|TypeError: fetch failed)\b/i.test(primary)) {
    const message = formatModelIssue(stage, provider, diagnostic?.status ? { status: diagnostic.status, message: primary } : primary);
    if (diagnostic?.outcome === "transport_error" || diagnostic?.outcome === "timeout") {
      const recovery = diagnostic.stage === "download" ? "已有结果时仅重试下载。" : "";
      return `${message} ${transportHint(diagnostic)} ${recovery}${formatDiagnostic(diagnostic)}`;
    }
    if (http?.[1] === "400" && stage === "video" && provider === "火山方舟")
      return message + " 请求参数不被当前模型接受，请核对首尾帧、比例和时长。";
    return message;
  }
  // Local conversion, checkpoints, paid-task uncertainty and cancellation have
  // specific recovery instructions and no authenticated model/HTTP stage.
  return primary.length > 700 ? primary.slice(0, 700) + "…" : primary;
}
