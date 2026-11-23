// Native-only address editor. Peer addresses stay out of component snapshots.
static void render_accessory(const char *id, uint32_t owner, unsigned kind, uint64_t now) {
  pet_accessory_view_t v = {0};
  char text[96], address[52];
  const char *title = kind == PET_ACCESSORY_BATTERY ? "配件电量" : "配件温度";
  if (!pet_accessory_view(owner, &v)) {
    render_component_unavailable(title, "请打开组件");
    return;
  }
  switch (v.status) {
  case PET_ACCESSORY_WAIT:
    snprintf(text, sizeof(text), "正在读取 · SW1 取消");
    break;
  case PET_ACCESSORY_BUSY:
    snprintf(text, sizeof(text), "蓝牙忙或不可用，请检查键盘模式");
    break;
  case PET_ACCESSORY_FAILED:
    snprintf(text, sizeof(text), "读取失败，请检查地址和公开服务");
    break;
  case PET_ACCESSORY_ADDRESS:
    snprintf(text, sizeof(text), "请输入设备的公开或静态随机地址");
    break;
  case PET_ACCESSORY_VALUE:
    if (kind == PET_ACCESSORY_BATTERY)
      snprintf(text, sizeof(text), "%d%%", v.value);
    else
      snprintf(text, sizeof(text), "%s%d.%02d °C", v.value < 0 ? "-" : "", abs(v.value) / 100,
               abs(v.value) % 100);
    break;
  default:
    snprintf(text, sizeof(text), "输入目标地址 · SW1 允许读取一次");
    break;
  }
  render_component_unavailable(title, text);
  char *end = address;
  for (unsigned i = 0; i < 12; i++) {
    if (i && !(i % 2))
      *end++ = ':';
    if (v.cursor == i)
      *end++ = '[';
    *end++ = "0123456789ABCDEF"[v.digits[i]];
    if (v.cursor == i)
      *end++ = ']';
  }
  snprintf(end, sizeof(address) - (size_t)(end - address), "  %s%s%s", v.cursor == 12 ? "[" : "",
           v.random ? "STATIC" : "PUBLIC", v.cursor == 12 ? "]" : "");
  draw_text_center(id, 320, 116, 600, rgb565(200, 210, 220), 1);
  draw_text_center(address, 320, 318, 600, rgb565(225, 238, 248), 1);
  draw_text_center("摇杆左右选位，上下修改 · 退出即清除", 320, 375, 600, rgb565(176, 186, 207), 1);
  pet_accessory_shown(owner, now);
}
