// Never interpolate a server response, request URL, endpoint label, or account value into feedback.
const stages = {
  asr: '语音识别（ASR）', tts: '语音合成（TTS）', chat: '对话大模型',
  image: '图片生成', video: '视频生成', catalog: '读取模型列表',
  storage: '本机配置', network: '云服务网络',
};
const providerNames = new Map([
  ['mimo', 'MiMo'], ['小米 mimo', 'MiMo'], ['xiaomi mimo', 'MiMo'],
  ['mimo-token-plan-cn', 'MiMo Token Plan'], ['mimo token plan', 'MiMo Token Plan'],
  ['豆包语音', '豆包语音'], ['volcengine', '火山引擎'], ['火山引擎', '火山引擎'],
  ['doubao', '豆包'], ['豆包', '豆包'], ['火山方舟（豆包）', '火山方舟'], ['火山方舟', '火山方舟'], ['ark', '火山方舟'],
  ['deepseek', 'DeepSeek'], ['自定义', '自定义'], ['custom', '自定义'], ['自定义端点', '自定义'],
  ['可灵', '可灵'], ['可灵 ai', '可灵'], ['kling', '可灵'], ['gpustack', 'GPUStack'],
]);

function accessHelp(stage, provider) {
  const volc = ['豆包语音', '火山引擎', '豆包', '火山方舟'].includes(provider);
  if (volc && stage === 'asr') return '请在豆包语音控制台检查“流式语音识别”的开通状态和语音 Key 的权限。';
  if (volc && stage === 'tts') return '请在豆包语音控制台检查“语音合成 2.0”的开通状态、音色和语音 Key 的权限。';
  if (volc && ['chat', 'image', 'video', 'catalog'].includes(stage)) return '请到火山方舟检查所选模型的开通状态，以及方舟 Key 所属项目的模型权限。';
  return '请在服务商控制台检查当前 Key 的权限和账号可用模型。';
}

const localReasons = [
  [/切换.*(?:Key|密钥)|新接口地址的 API Key/, '服务或接口已更换，请填写新服务的 Key。'],
  [/(?:请先?(?:填写|保存)|请先保存或填写|没有已保存的|尚未配置|缺少|未配置).{0,22}(?:API Key|密钥)/i, '当前服务还没有可用的 API Key，请到对应服务设置中填写并保存。'],
  [/配置密钥缺失|未找到原配置密钥|系统凭据密钥损坏|无法读取系统凭据|无法验证原配置|无法解密/, '无法解锁已有配置。请使用原系统账号，并检查系统凭据存储是否可用。保留原配置文件。'],
  [/请选择普通证书文件|只允许导入 CA 公共证书|PEM 公共证书|PEM 格式公共证书/, '请选择 PEM 格式的 CA 公共证书文件，文件中不能含私钥。'],
  [/代理仅支持|代理地址无效|代理配置无效/, '请填写 HTTP 或 HTTPS 代理地址和端口，去掉路径、查询参数和内嵌密码。'],
  [/接口地址(?:无效|为空|格式)|地址中请勿携带凭据|地址缺少主机|地址不能包含凭据|服务需使用 HTTPS/, '请核对接口地址。远程服务使用 HTTPS，地址中请勿包含 Key、账号或密码。'],
  [/模型目录不完整|模型目录过大|模型列表不完整/, '服务返回的模型列表不完整。可手动填写服务商提供的模型 ID。'],
  [/请填写语音识别模型和合成模型/, '请分别填写识别模型 ID 和合成模型 ID，再保存语音配置。'],
  [/未配置模型名称|未填写模型名称/, '请在当前服务中填写模型 ID 后重新测试。'],
  [/音色设计描述/, '所选模型需要音色设计描述。请填写描述或选择支持预设音色的模型。'],
  [/未返回对话流|未返回.*流式/, '所选模型未返回流式对话。请核对接口和模型的流式输出能力。'],
  [/对话流提前结束|工具调用流提前结束|未正常完成本轮输出/, '服务提前结束了回复。请检查连接后重试。'],
  [/输出达到长度上限/, '模型输出达到长度上限，请缩短问题或分步提问。'],
  [/响应超过大小上限|音频超过大小上限|JSON 响应过大|对话流单行超过安全上限/, '服务返回的数据超过客户端大小限制。请缩短输入或使用较短的输出。'],
  [/握手校验失败|协议无效|响应协议版本无效|响应帧头无效/, '服务返回了不兼容的协议响应。请核对接口类型与版本。'],
  [/响应格式无效|响应 JSON .*无效|返回无效 JSON|对话流编码无效|对话流数据损坏/, '返回数据的格式无法解析。请核对接口类型和所选模型。'],
  [/未返回可用文本|未转写测试音频|没有返回.*文本|没有返回.*转写|未返回转写|empty transcript|无有效语音|没有识别到/, '服务没有返回有效文本。'],
  [/模型.{0,6}不存在|model[_ ]not[_ ]found|invalid[_ ]model/i, '服务找不到所选模型。请核对模型 ID 和当前账号可用的模型。'],
  [/音色.{0,8}(无效|不支持|不存在)|invalid[_ ](?:speaker|voice)/i, '所选音色不可用。请核对音色 ID 与合成模型是否匹配。'],
  [/解析.*音频失败|invalid wav|不是 WAV|WAV (?:数据块不完整|必须使用 PCM|缺少|采样率异常|包含不完整|长度与响应不一致)|缺少音频|音频编码无效|音频内容为空|未返回.*音频|audio.*missing/i, '服务没有返回可播放的音频。请核对合成模型及响应格式。'],
  [/等待音频超时|45000081/, '服务等待音频超时。请检查录音输入，重新说一段话。'],
  [/quota_exceeded|insufficient_quota|余额不足|额度耗尽|配额已用完/i, '服务报告可用额度不足。请检查账号余额、配额和套餐。'],
  [/TLS|证书验证|certificate|证书校验/i, '无法验证服务证书。请检查系统时间和云服务网络中的 CA 设置。'],
  [/DNS|域名解析/i, '无法解析服务域名。请核对地址并检查 DNS 或代理。'],
  [/timeout|timed out|超时/i, '等待服务响应超时。请检查网络或代理后重试。'],
  [/连接已关闭|连接中断|连接被拒绝|connection refused|无法连接|连接失败|网络不可达|failed to fetch/i, '连接服务失败。请检查服务地址、网络和代理。'],
];

function httpReason(status, stage, provider) {
  if (status === 401) {
    const volc = ['豆包语音', '火山引擎', '豆包', '火山方舟'].includes(provider);
    const key = volc && ['asr', 'tts'].includes(stage) ? '豆包语音 Key' : volc && stage === 'chat' ? '火山方舟 Key' : '当前服务的 API Key';
    return '服务未接受本次凭据。请核对'+key+'是否有效，并确认它属于所选接口。';
  }
  if (status === 403) return '服务拒绝了本次访问。'+accessHelp(stage, provider);
  if (status === 404 || status === 405) return '服务找不到所请求的接口或不支持该请求。请核对接口路径、模型 ID 和该模型支持的能力。';
  if (status === 402) return '服务要求付费或补充额度。请检查账号余额和套餐。';
  if (status === 429) return '服务限制了本次请求。请稍后重试，并检查并发限制和可用额度。';
  if (status === 407) return '代理服务器要求身份验证。请检查系统代理或云服务网络设置。';
  if (status === 408 || status === 504) return '请求超时。请检查网络，稍后重试。';
  if (status >= 500) return '服务端或网关处理请求时出错。请稍后重试。';
  if (status === 413) return '请求内容超过服务大小限制。请缩短输入或减小文件后重试。';
  if (status === 400 || status === 422 || status === 415) return '服务未接受请求参数或内容格式。请核对所选模型和输入格式。';
  return '服务未接受本次请求。请检查服务配置。';
}

export function formatModelIssue(stage, provider, error) {
  const name = providerNames.get(String(provider || '').toLowerCase().trim());
  const raw = typeof error === 'string' ? error : typeof error?.message === 'string' ? error.message : '';
  const text = raw.slice(0, 3000).trim();
  const tag = /^\[(ASR|TTS|LLM|CREDENTIALS|NETWORK)\]\s*/.exec(text);
  if (tag) stage = {ASR:'asr', TTS:'tts', LLM:'chat', CREDENTIALS:'storage', NETWORK:'network'}[tag[1]];
  const label = Object.hasOwn(stages, stage) ? stages[stage] : '模型服务';
  const title = name ? name+' · '+label : label;
  const detail = tag ? text.slice(tag[0].length) : text;
  const business = /^code=(400|401|402|403|404|429|500|502|503|504|45000000|45000001|45000002|45000081|55000000|55000031|invalid_api_key|permission_denied|model_not_found|insufficient_quota|rate_limit_exceeded|invalid_request_error|unknown)(?=\s|$)/.exec(detail);
  if (business) {
    const code = business[1];
    const statusByCode = {invalid_api_key:401, permission_denied:403, rate_limit_exceeded:429, invalid_request_error:400};
    const status = statusByCode[code] || (/^[45]\d\d$/.test(code) ? Number(code) : null);
    const reason = status ? httpReason(status, stage, name)
      : localReasons.find(([pattern]) => pattern.test(code))?.[1] || '服务未完成请求。请核对当前模型和资源配置后重试。';
    return code === 'unknown' ? title+'　'+reason : title+'（服务错误码 '+code+'）　'+reason;
  }
  const closed = /^WS (\d{4})(?=\s|$)/.exec(detail);
  if (closed) return title+'（WebSocket 关闭码 '+closed[1]+'）　服务在操作完成前关闭了连接，请重新连接后再试。';
  const http = text.match(/\bHTTP(?:\/\d(?:\.\d)?)?\s*(?:状态码?|status(?: code)?)?\s*[:：=]?\s*([45]\d\d)\b/i);
  const status = http ? Number(http[1]) : error?.status;
  if (Number.isInteger(status) && status >= 400 && status <= 599) {
    const reason = stage === 'network' && status === 403
      ? '已收到 HTTPS 响应，但本次无凭据的连通性检测被拒绝。请检查网络策略，并在对应服务中测试 Key。'
      : httpReason(status, stage, name);
    return title+'（HTTP '+status+'）　'+reason;
  }
  const match = localReasons.find(([pattern]) => pattern.test(text));
  if (match) return title+'　'+match[1];
  if (stage === 'storage') return title+'　未能完成本机配置操作。请检查应用的数据目录权限和可用空间，再重试。';
  if (stage === 'catalog') return title+'　未能取得模型列表。请重试，也可直接填写模型 ID。';
  return title+'　此次操作未完成，当前信息不足以确定原因。请在“模型配置”中单独测试对应服务。';
}
