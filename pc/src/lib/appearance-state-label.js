// Stable state IDs are used on disk and on the device. These names are for display.
const names = {
  welcome: "欢迎", working: "工作", waiting_user: "等待操作", done: "任务完成", error: "出错",
  idle: "休息", "idle.default": "静候", "idle.playing": "玩耍", "idle.wandering": "散步",
  "idle.jumping": "跳跃", "idle.begging": "撒娇", "idle.daydreaming": "发呆",
  "idle.eating": "进食", "idle.reading": "阅读", "idle.traveling": "旅行",
  "working.thinking": "思考", "working.typing": "打字", "working.browsing": "浏览",
  "touch.lick": "亲近回应", "touch.what": "好奇回应", "touch.right": "触碰右侧", "touch.left": "触碰左侧",
  permission_request: "请求审批", permission_approved: "审批通过", permission_denied: "审批拒绝",
  permission_expired: "审批超时", session_terminated: "会话停止",
};

export function appearanceStateLabel(id) {
  return Object.hasOwn(names, id) ? names[id] : id;
}
