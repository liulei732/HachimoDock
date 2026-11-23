/* Shared 640 x 480 logical surface. Password text never enters the view. */
static void render_settings_page(const pet_settings_view_t *settings) {
  const uint16_t paper = rgb565(247, 240, 230);
  const uint16_t panel = rgb565(255, 251, 244);
  const uint16_t ink = rgb565(55, 43, 35);
  const uint16_t muted = rgb565(112, 90, 74);
  const uint16_t outline = rgb565(226, 208, 187);
  const uint16_t accent = rgb565(159, 74, 35);
  const uint16_t faint = rgb565(247, 222, 198);
  const uint16_t green = rgb565(48, 113, 73);

  fill_rect(0, 0, PET_P4_UI_WIDTH, PET_P4_UI_HEIGHT, paper);
  fill_round_rect(24, 16, 92, 54, 13, faint);
  draw_text_center("返回", 70, 33, 80, ink, 1);

  if (settings->screen == PET_SETTINGS_HOME) {
    draw_text_line_vcenter("系统设置", 139, 16, 54, 450, ink, 2, true);
    static const char *const labels[] = {"WIFI 配置", "电脑配对", "时区设置", "蓝牙键盘", "组件社区", "声音与显示"};
    const unsigned count = sizeof(labels)/sizeof(*labels);
    const int step = pet_settings_home_step(settings);
    for (unsigned i = 0; i < count; ++i) {
      int y = 103 + step * (int)(i > 4 ? 4 : i);
      int x = i == 5 ? 328 : 24, w = i >= 4 ? 288 : 592;
      fill_round_rect_outline(x, y, w, step - 16, 17, panel, settings->selected == i ? accent : outline);
      draw_text_line_vcenter(labels[i], x+18, y, step - 16, w-64, ink, 2, true);
      draw_text_center(">", x+w-29, y + (step - 16) / 3, 27, accent, 2);
    }
    return;
  }

  if (settings->confirm_remove || settings->screen == PET_SETTINGS_AV || settings->screen == PET_SETTINGS_BLE) {
    const bool remove = settings->confirm_remove, av = settings->screen == PET_SETTINGS_AV;
    const char *title = remove ? (settings->screen == PET_SETTINGS_FEED ? "移除服务授权？" : "解除配对？") :
                         av ? "声音与显示" : "蓝牙键盘";
    draw_text_line_vcenter(title,139,16,54,450,ink,2,true);
    const char *left = "取消", *right = "确认移除", *message = "";
    unsigned base = 0;
    if (av) {
      for (unsigned i=0; i<2; ++i) {
        int value = i ? settings->brightness : settings->volume, y = 130 + (int)i*80;
        char label[64];
        const char *storage = i ? (settings->brightness_saved ? " 已保存" : "") :
          settings->volume_storage == PET_AV_SAVING ? " 待保存" :
          settings->volume_storage == PET_AV_SAVE_FAILED ? " 保存失败" :
          settings->volume_storage == PET_AV_SAVED ? " 已保存" : "";
        if (value >= 0) snprintf(label,sizeof(label),"%s  %d%%%s",i?"亮度":"音量",value,storage);
        else snprintf(label,sizeof(label),"%s",i?"亮度不可调":"音量暂不可用");
        draw_text_center(label,320,y+17,354,ink,2);
        for (int k=0; k<2; ++k) {
          int x=k?510:24;
          fill_round_rect_outline(x,y,106,60,13,panel,settings->selected==i?accent:outline);
          draw_text_center(k?"+":"-",x+53,y+17,96,value<0?muted:accent,2);
        }
      }
      left="保存"; right="刷新"; base=2;
      message=settings->av_pending?"正在读取或保存":settings->av_failed?"操作未完成，请刷新":
              settings->av_applied?"已应用":"左右调节，保存后生效";
    } else if (!remove) {
      left=settings->ble_enabled?"关闭键盘":"启用键盘";
      right=settings->ble_paired?"解除配对":settings->ble_pairing?"取消配对":"开始配对";
      message=settings->request_busy?"设置失败，请重试":settings->ble_available<0?"蓝牙暂不可用":
        !settings->ble_available?"正在准备蓝牙":settings->ble_connected?"键盘已连接":
        settings->ble_paired?"已配对，等待连接":settings->ble_pairing?"在电脑蓝牙设置中选 HachimiDock":
        settings->ble_enabled?"选择开始配对":"键盘已关闭";
      if (settings->ble_code) {
        char code[12];
        snprintf(code,sizeof(code),"%06u",settings->ble_code);
        draw_text_center(code,320,230,592,accent,2);
      }
    }
    draw_text_line_vcenter(message,24,av?300:100,62,592,muted,1,true);
    for (unsigned i=0; i<2; ++i) {
      int x=i?328:24;
      bool focused=settings->selected==base+i;
      bool enabled=remove || av || (settings->ble_available>0 && (!i || settings->ble_enabled || settings->ble_paired));
      fill_round_rect_outline(x,392,288,60,13,focused?faint:panel,focused?accent:outline);
      draw_text_center(i?right:left,x+144,412,270,enabled?ink:muted,1);
    }
    return;
  }

  if (settings->screen == PET_SETTINGS_COMMUNITY) {
    const pet_community_view_t *v=&settings->community;
    const bool confirm=settings->community_confirm;
    draw_text_line_vcenter(v->installed?"我的社区组件":"组件社区",139,16,54,450,ink,2,true);
    char title[148];
    snprintf(title,sizeof(title),"%s  %s",v->title,v->version);
    draw_text_line_vcenter(title,24,85,54,592,ink,2,true);
    char index[72]="";
    if (v->count) snprintf(index,sizeof(index),"%u / %u%s    %s",v->index+1,v->count,v->more?" +":"",v->id);
    draw_text_line_vcenter(index,24,140,32,592,muted,1,true);
    draw_text_line_vcenter(v->status?v->status:"",24,171,30,592,muted,1,true);
    const char *prompt=confirm && settings->community_delete?"删除组件并保留存档？":
      !v->count || v->installed?"":v->privileges?(v->added_save?"新增权限：保存组件本地数据":"权限：保存组件本地数据"):"权限：无";
    draw_text_line_vcenter(prompt,24,203,32,592,accent,1,true);
    const char *labels[]={"上一个","下一个",v->remove?"更新":"安装","删除",v->installed?"社区目录":"我的组件","刷新"};
    if (confirm) { labels[0]="取消"; labels[1]=settings->community_delete?"确认删除":"确认安装"; }
    for (unsigned i=0;i<(confirm?2u:6u);i++) {
      int x=24+(int)(i%2)*304, y=241+(int)(i/2)*70;
      bool enabled=!v->busy && (confirm || (i<2?v->count!=0:i==2?v->install:i==3?v->remove:true));
      fill_round_rect_outline(x,y,288,60,14,settings->selected==i?faint:panel,settings->selected==i?accent:outline);
      draw_text_center(labels[i],x+144,y+19,270,enabled?ink:muted,1);
    }
    return;
  }

  if (settings->screen == PET_SETTINGS_TIMEZONE) {
    const bool automatic = settings->timezone_draft == PET_WIFI_TZ_AUTO;
    const int offset = automatic ? settings->timezone_effective : settings->timezone_draft;
    const int magnitude = offset < 0 ? -offset : offset;
    char utc[16];
    snprintf(utc, sizeof(utc), "UTC%c%02d:%02d", offset < 0 ? '-' : '+',
             magnitude / 60, magnitude % 60);
    draw_text_line_vcenter("时区设置", 139, 16, 54, 450, ink, 2, true);
    draw_text_center(automatic ? "跟随电脑（默认 UTC）" : "固定时区", 320, 105, 592, muted, 1);
    fill_round_rect_outline(24, 160, 106, 72, 13, panel, outline);
    fill_round_rect_outline(510, 160, 106, 72, 13, panel, outline);
    draw_text_center("-", 77, 181, 96, accent, 2);
    draw_text_center("+", 563, 181, 96, accent, 2);
    draw_text_center(utc, 320, 181, 360, ink, 2);
    fill_round_rect_outline(24, 260, 592, 70, 13, panel, outline);
    draw_text_center(automatic ? "使用固定时区" : "跟随电脑", 320, 284, 540, ink, 1);
    fill_round_rect_outline(24, 392, 592, 60, 13, faint, accent);
    draw_text_center("保存", 320, 412, 540, ink, 1);
    return;
  }

  if (settings->screen == PET_SETTINGS_LAN) {
    const pet_lan_pair_view_t *lan = &settings->lan;
    draw_text_line_vcenter("电脑配对", 139, 16, 54, 450, ink, 2, true);
    draw_text_line_vcenter(lan->busy ? "正在配对" : lan->paired ? "已配对" :
      !lan->ip[0] ? "请先连接 WIFI" : lan->ready ? "等待电脑配对" : "配对状态暂不可用",
      25, 81, 46, 590, lan->paired ? green : muted, 1, true);
    if (lan->ip[0]) draw_text_line_vcenter(lan->ip, 25, 131, 45, 590, ink, 2, true);
    draw_text_line_vcenter(lan->paired ? "配对已保存，电脑可自动连接" : "在电脑设置中发现并选择本设备",
      25, 195, 40, 590, muted, 1, true);
    draw_text_line_vcenter(lan->paired ? "更换电脑前，请先解除配对" : "电脑确认后即可完成，无需设备操作",
      25, 245, 40, 590, muted, 1, true);
    if (lan->failed && !lan->paired)
      draw_text_line_vcenter("配对未完成，请在电脑重试",25,295,40,590,accent,1,true);
    if (lan->paired && !lan->busy) {
      fill_round_rect_outline(24, 392, 592, 60, 13, faint, outline);
      draw_text_center("解除配对", 320, 412, 540, ink, 1);
    }
    return;
  }

  if (settings->screen == PET_SETTINGS_FEED) {
    draw_text_line_vcenter("连接服务账号", 139, 16, 54, 450, ink, 2, true);
    draw_text_line_vcenter(settings->feed_host, 25, 90, 48, 590, ink, 1, true);
    draw_text_line_vcenter(settings->feed_target, 25, 144, 40, 590, muted, 1, true);
    const char *status = !settings->feed_can_save ? "设备安全存储尚未配置" :
      settings->request_busy ? "操作失败，请重试" :
      settings->feed_result == PET_PRIVATE_BUSY ? "正在保存" :
      settings->feed_result == PET_PRIVATE_SET ? "已保存" :
      settings->feed_result == PET_PRIVATE_ABSENT ? "已清除" :
      settings->feed_result == PET_PRIVATE_STORAGE_ERROR ? "凭证存储已满或不可用" :
      "授权后在此组件查看私人数据";
#if PET_P4_SCOPED_WRITE
    if (pet_feed_is_write(settings->feed_kind) && settings->feed_can_save && !settings->request_busy) {
      if (settings->feed_result == PET_WRITE_UNKNOWN) status = "结果未知，先核对服务，再移除授权";
      else if (settings->feed_result == PET_WRITE_SPENT) status = "上次已确认，请重新授权";
      else if (settings->feed_result == PET_WRITE_ACCEPTED) status = "服务已确认接收";
      else if (settings->write_submitted) status = settings->feed_result == PET_PRIVATE_BUSY ?
        "正在发送" : "未发送，请检查授权和网络";
      else if (!settings->feed_submitted) status = "授权仅限本次发送";
    }
#endif
    draw_text_line_vcenter(status, 25, 200, 44, 590, accent, 1, true);
    if (settings->feed_can_save) {
      static const char *const actions[] = {"连接账号", "移除授权"};
      const unsigned count = pet_settings_feed_actions(settings);
      for (unsigned i = 0; i < count; ++i) {
        int y = 280 + 70 * (int)i;
        fill_round_rect_outline(24, y, 592, 58, 13, panel, settings->selected == i ? accent : outline);
        const char *action =
#if PET_P4_SCOPED_WRITE
          i == 2 ? settings->write_label :
#endif
          actions[i];
        draw_text_center(action, 320, y + 20, 540, ink, 1);
      }
    }
    return;
  }

  if (settings->screen == PET_SETTINGS_PASSWORD || settings->screen == PET_SETTINGS_FEED_ENTRY) {
    const bool feed = settings->screen == PET_SETTINGS_FEED_ENTRY;
    const char *label = !feed ? "输入密码" : settings->feed_username ? "服务账号" :
        (settings->feed_kind == PET_PRIVATE_MQTT || settings->feed_kind == PET_MQTT_PUBLISH) ? "服务密码" : "输入 Token";
    draw_text_line_vcenter(label, 139, 16, 54, 450, ink, 2, true);
    draw_text_line_vcenter(feed ? settings->feed_host : settings->target.ssid, 27, 71, 29, 586, muted, 1, true);
    fill_round_rect_outline(24, 102, 592, 43, 12, panel, outline);
    if (settings->password_length) {
      char masked[35];
      unsigned length = settings->password_length;
      if (length >= sizeof(masked)) length = sizeof(masked) - 1;
      memset(masked, '*', length);
      masked[length] = 0;
      draw_text_line_vcenter(masked, 39, 102, 43, 455, ink, 1, true);
    } else draw_text_line_vcenter(label, 39, 102, 43, 455, muted, 1, true);
    if (settings->entry_full)
      draw_text_line_vcenter("已达上限", 455, 102, 43, 140, accent, 1, true);
    else if (settings->short_password)
      draw_text_line_vcenter(feed ? "请核对后重试" : "至少 8 位", 455, 102, 43, 140, accent, 1, true);
    else if (settings->request_busy)
      draw_text_line_vcenter("请稍候", 496, 102, 43, 100, accent, 1, true);
    for (unsigned row = 0; row < PET_SETTINGS_KEY_ROWS; ++row) {
      unsigned columns = pet_settings_key_columns(row);
      int width = PET_SETTINGS_KEY_WIDTH / (int)columns;
      for (unsigned col = 0; col < columns; ++col) {
        int x = PET_SETTINGS_KEY_LEFT + (int)col * width + PET_SETTINGS_KEY_GAP / 2;
        int y = PET_SETTINGS_KEY_TOP + (int)row * PET_SETTINGS_KEY_ROW_HEIGHT;
        bool focused = row == settings->key_row && col == settings->key_column;
        bool connect = row == 4 && col == 4;
        bool mode_selected = row == 4 && ((col == 0 && settings->keyboard == 1) ||
                                             (col == 1 && settings->keyboard == 2));
        int key_height = PET_SETTINGS_KEY_ROW_HEIGHT - PET_SETTINGS_KEY_GAP;
        fill_round_rect_outline(x, y, width - PET_SETTINGS_KEY_GAP, key_height, 10,
          connect ? accent : (focused || mode_selected) ? faint : panel,
          (focused || mode_selected) ? accent : outline);
        char ascii[2];
        const char *key = feed && connect ? (settings->feed_username ? "下一步" : "保存") :
          pet_settings_key_label(settings->keyboard, row, col, ascii);
        draw_text_center(key, x + (width - PET_SETTINGS_KEY_GAP) / 2, y + (row == 4 ? 20 : 14),
                         width - PET_SETTINGS_KEY_GAP - 4, connect ? panel : ink, row == 4 ? 1 : 2);
      }
    }
    return;
  }

  const pet_wifi_snapshot_t *wifi = &settings->wifi;
  const bool saved_list = settings->screen == PET_SETTINGS_WIFI_SAVED;
  const bool network = settings->screen == PET_SETTINGS_WIFI_NETWORK;
  const bool confirm = settings->screen == PET_SETTINGS_WIFI_FORGET;
  draw_text_line_vcenter(confirm ? "忘记网络？" : saved_list || network ? "已保存网络" : "WIFI配置",
                         139, 16, 54, 450, ink, 2, true);
  const char *status = pet_settings_wifi_status(settings);
  draw_text_line_vcenter(status, 26, 74, 40, wifi->connected[0] ? 307 : 586,
                         wifi->status == PET_WIFI_CONNECTED ? green : muted, 1, true);
  if (wifi->connected[0])
    draw_text_right(wifi->connected, 612, 85, 284, green, 1);
  if (network || confirm) {
    draw_text_line_vcenter(settings->target.ssid, 26, 106, 40, 450, ink, 1, true);
    draw_text_right(settings->target.security == PET_WIFI_OPEN ? "开放" :
      settings->target.security == PET_WIFI_WPA3 ? "WPA3" : "WPA2", 612, 118, 120, muted, 1);
    if (settings->saved_target < 0) return;
    const bool auto_join = wifi->saved[settings->saved_target].auto_join;
    const char *const actions[] = {"连接", auto_join ? "[x] 自动连接" : "[ ] 自动连接", "忘记网络", "修改密码并连接"};
    const char *const confirmation[] = {"取消", "确认忘记"};
    const unsigned count = confirm ? 2 : 3 + (settings->target.security != PET_WIFI_OPEN);
    for (unsigned row = 0; row < count; ++row) {
      const int y = PET_SETTINGS_WIFI_ACTION_TOP + (int)row * PET_SETTINGS_WIFI_ACTION_STEP;
      const bool focused = row == settings->selected;
      fill_round_rect_outline(24, y, 592, PET_SETTINGS_WIFI_ACTION_HEIGHT, 13,
                              focused ? faint : panel, focused ? accent : outline);
      draw_text_line_vcenter(confirm ? confirmation[row] : actions[row], 43, y,
                             PET_SETTINGS_WIFI_ACTION_HEIGHT, 540, ink, 1, true);
    }
    return;
  }

  const unsigned list_count = saved_list ? wifi->saved_count : wifi->count;
  for (unsigned row = 0; row < PET_SETTINGS_WIFI_ROWS; ++row) {
    const unsigned index = settings->list_start + row;
    if (index >= list_count) break;
    const bool focused = index == settings->selected;
    const int y = 126 + (int)row * 64;
    fill_round_rect_outline(24, y, 592, 58, 13, focused ? faint : panel, focused ? accent : outline);
    if (saved_list) {
      const pet_wifi_saved_network_t *entry = &wifi->saved[index];
      draw_text_line_vcenter(entry->ssid, 43, y, 58, 348, ink, 1, true);
      draw_text_line_vcenter(entry->security == PET_WIFI_OPEN ? "开放" :
        entry->security == PET_WIFI_WPA3 ? "WPA3" : "WPA2", 398, y, 58, 58, muted, 1, true);
      draw_text_right(entry->current ? "已连接" : entry->auto_join ? "自动连接" : "手动连接",
                      600, y + 20, 132, entry->current ? green : muted, 1);
      continue;
    }
    const pet_wifi_ap_t *ap = &wifi->aps[index];
    draw_text_line_vcenter(ap->ssid, 43, y, 58, 420, ink, 1, true);
    draw_text_line_vcenter(ap->security == PET_WIFI_OPEN ? "开放" :
      ap->security == PET_WIFI_UNSUPPORTED ? "不支持" : "加密", 484, y, 58, 68, muted, 1, true);
    unsigned strength = ap->rssi >= -55 ? 4 : ap->rssi >= -68 ? 3 : ap->rssi >= -80 ? 2 : 1;
    for (unsigned bar = 0; bar < 4; ++bar)
      fill_rect(560 + (int)bar * 9, y + 39 - (int)bar * 5, 5, 5 + (int)bar * 5,
                bar < strength ? accent : outline);
  }
  fill_round_rect_outline(24, 395, 186, 60, 13,
    settings->selected == list_count ? faint : panel, outline);
  draw_text_center(saved_list ? "扫描结果" : "重新扫描", 117, 416, 164, ink, 1);
  if (!saved_list) {
    fill_round_rect_outline(230, 395, 185, 60, 13,
      settings->selected > wifi->count ? faint : panel, outline);
    draw_text_center("已保存网络", 322, 416, 164, ink, 1);
  }
  if (list_count > PET_SETTINGS_WIFI_ROWS) {
    const unsigned current_page = settings->list_start / PET_SETTINGS_WIFI_ROWS + 1;
    const unsigned pages = (list_count + PET_SETTINGS_WIFI_ROWS - 1) / PET_SETTINGS_WIFI_ROWS;
    char navigation[48];
    snprintf(navigation, sizeof(navigation), "%s %u/%u",
             current_page < pages ? "下一页" : "回到第1页", current_page, pages);
    fill_round_rect_outline(435, 395, 181, 60, 13, panel, outline);
    draw_text_center(navigation, 525, 416, 163, ink, 1);
  } else if (list_count) {
    char count[32];
    snprintf(count, sizeof(count), "共 %u 个网络", list_count);
    draw_text_center(count, 525, 416, 163, muted, 1);
  }
}
