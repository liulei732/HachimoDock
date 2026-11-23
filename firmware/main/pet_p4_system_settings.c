#include "pet_p4_system_settings.h"

#if PET_P4_SYSTEM_WIFI
#include <stdio.h>

#include "pet_p4_audio.h"
#include "pet_p4_ble_shortcut.h"
#include "pet_p4_ota.h"
#include "pet_p4_miniapp.h"
#include "mbedtls/platform_util.h"
#include "pet_p4_wifi_clock.h"
#include "pet_p4_wifi_data.h"
#include "pet_p4_mqtt_feed.h"

static pet_settings_view_t page;
static char return_page[PET_P4_PAGE_MAX];
static char password[PET_PRIVATE_TOKEN_MAX + 1];
static pet_private_feed_t selected_feed;
static pet_private_secret_t feed_secret;
static bool page_open;
static pet_settings_screen_t network_parent;
static int applied_volume, applied_brightness;
static uint32_t av_ticket;
static bool av_save;

static void request_av(bool save) {
  if (page.av_pending) return;
  if (!++av_ticket) ++av_ticket;
  av_save = save;
  page.av_applied = false;
  page.av_pending = pet_settings_av_queue(av_ticket);
  page.av_failed = !page.av_pending;
}

bool pet_settings_av_claim(uint32_t ticket, int *volume, int *brightness) {
  if (!page_open || page.screen != PET_SETTINGS_AV || !page.av_pending || ticket != av_ticket) return false;
  *volume = av_save && applied_volume != page.volume ? page.volume : -1;
  *brightness = av_save && applied_brightness != page.brightness ? page.brightness : -1;
  return true;
}

void pet_settings_av_disk(int saved, int pending, bool failed) {
  if (!page_open || page.screen != PET_SETTINGS_AV || page.av_pending) return;
  page.volume_storage = PET_AV_UNCONFIRMED;
  if (page.volume < 0 || page.volume != applied_volume) return;
  if (pending == page.volume) page.volume_storage = failed ? PET_AV_SAVE_FAILED : PET_AV_SAVING;
  else if (pending < 0 && saved == page.volume) page.volume_storage = PET_AV_SAVED;
}

void pet_settings_av_finish(uint32_t ticket, int volume, int brightness, bool ok, bool brightness_saved) {
  if (!page_open || page.screen != PET_SETTINGS_AV || !page.av_pending || ticket != av_ticket) return;
  page.volume_storage = PET_AV_UNCONFIRMED;
  page.brightness_saved = brightness_saved;
  page.volume = applied_volume = volume;
  page.brightness = applied_brightness = brightness;
  page.av_pending = false;
  page.av_failed = !ok;
  page.av_applied = ok && av_save;
}

static void av_input(pet_settings_input_t input) {
  if (page.av_pending) return;
  if (input == PET_SETTINGS_UP && page.selected) --page.selected;
  if (input == PET_SETTINGS_DOWN && page.selected < 3) ++page.selected;
  if (input == PET_SETTINGS_ENTER && page.selected >= 2) request_av(page.selected == 2);
  if (page.selected >= 2 || (input != PET_SETTINGS_LEFT && input != PET_SETTINGS_RIGHT)) return;
  int *value = page.selected ? &page.brightness : &page.volume;
  if (*value < 0) return;
  if (page.selected) page.brightness_saved = false;
  else page.volume_storage = PET_AV_UNCONFIRMED;
  *value += input == PET_SETTINGS_LEFT ? -10 : 10;
  int minimum = page.selected ? 10 : 0;
  if (*value < minimum) *value = minimum;
  if (*value > 100) *value = 100;
  page.av_applied = page.av_failed = false;
}

static void clear_password(void) {
  volatile char *p = password;
  for (unsigned i = 0; i < sizeof(password); ++i) p[i] = 0;
  page.password_length = 0;
  page.entry_full = false;
}

bool pet_settings_open(pet_p4_runtime_state_t *state, unsigned long long ts_ms) {
  if (!state || pet_settings_active(state) || !pet_p4_is_wifi6_hardware() ||
      state->approval_active || state->asset_transfer_active || pet_p4_ota_transfer_active() ||
      state->session_voice_active || pet_p4_audio_active()) return false;
  const bool component = !strcmp(state->screen_page, "app");
  if (!component && strcmp(state->screen_page, "main") && strcmp(state->screen_page, "components"))
    return false;
  clear_password();
  memset(&page, 0, sizeof(page));
  pet_wifi_endpoint_t endpoint;
  if (component && pet_p4_miniapp_private_feed(&selected_feed, &endpoint)) {
    page.screen = PET_SETTINGS_FEED;
    page.feed_can_save = pet_private_storage_ready();
    page.feed_kind = selected_feed.kind;
    snprintf(page.feed_host, sizeof(page.feed_host), "%s", endpoint.host);
    const bool mqtt = page.feed_kind == PET_PRIVATE_MQTT || page.feed_kind == PET_MQTT_PUBLISH;
#if PET_P4_SCOPED_WRITE
    if (pet_feed_is_write(page.feed_kind)) {
      snprintf(page.feed_host, sizeof(page.feed_host), "%s %.*s:%u", mqtt ? "PUBLISH" : "POST",
               32, endpoint.host, selected_feed.port);
      snprintf(page.write_label, sizeof(page.write_label), "发送一次 %s", selected_feed.value);
    }
#endif
    const char *target = mqtt ? selected_feed.topic : endpoint.path;
    snprintf(page.feed_target, sizeof(page.feed_target), "%.*s", (int)sizeof(page.feed_target) - 1, target);
  }
  page_open = true;
  snprintf(return_page, sizeof(return_page), "%s", state->screen_page);
  snprintf(state->screen_page, sizeof(state->screen_page), "%s", PET_SETTINGS_PAGE);
  state->last_update_ms = ts_ms;
  return true;
}

static unsigned wifi_list_count(void) {
  return page.screen == PET_SETTINGS_WIFI_SAVED ? page.wifi.saved_count : page.wifi.count;
}

static void update_list(void) {
  if (page.screen != PET_SETTINGS_WIFI && page.screen != PET_SETTINGS_WIFI_SAVED &&
      page.screen != PET_SETTINGS_WIFI_NETWORK && page.screen != PET_SETTINGS_WIFI_FORGET) return;
  pet_device_wifi_snapshot(&page.wifi);
  if (page.screen == PET_SETTINGS_WIFI_NETWORK || page.screen == PET_SETTINGS_WIFI_FORGET) {
    page.saved_target = -1;
    for (unsigned i = 0; i < page.wifi.saved_count; ++i) {
      const pet_wifi_saved_network_t *entry = &page.wifi.saved[i];
      if (entry->security == page.target.security && !strcmp(entry->ssid, page.target.ssid)) {
        page.saved_target = (int)i;
        break;
      }
    }
  }
  if (page.screen != PET_SETTINGS_WIFI && page.screen != PET_SETTINGS_WIFI_SAVED) return;
  const unsigned count = wifi_list_count();
  const unsigned limit = count + (page.screen == PET_SETTINGS_WIFI);
  if (page.selected > limit) page.selected = limit;
  if (page.selected < count)
    page.list_start = page.selected / PET_SETTINGS_WIFI_ROWS * PET_SETTINGS_WIFI_ROWS;
  else if (page.list_start >= count)
    page.list_start = 0;
}

const char *pet_settings_wifi_status(const pet_settings_view_t *settings) {
  const pet_wifi_snapshot_t *wifi = &settings->wifi;
  if (wifi->status == PET_WIFI_SCANNING) return "正在扫描";
  if (wifi->status == PET_WIFI_CONNECTING) return "正在连接";
  if (wifi->request_pending) return "正在处理";
  if (wifi->status == PET_WIFI_REBOOT_REQUIRED) return "无线初始化中断，请重启设备";
  if (wifi->status == PET_WIFI_INIT_RETRY) return "无线未就绪，将自动重试";
  if (wifi->status == PET_WIFI_UNAVAILABLE) return "无线未就绪，请重新扫描";
  if (settings->request_busy) return "设备忙，请重试";
  if (wifi->saved_update_failed) return "保存设置失败";
  if (wifi->status == PET_WIFI_LIST_FULL) return "已满，请先忘记一个网络";
  if (settings->unsupported) return "此网络暂不支持";
  if (wifi->status == PET_WIFI_SCAN_FAILED) return "扫描失败，请重试";
  if (wifi->status == PET_WIFI_FAILED) {
    if (wifi->join_stage == PET_WIFI_JOIN_ADDRESS_TIMEOUT) return "获取地址超时，请检查路由器";
    switch (wifi->join_reason) {
      case 201: return "未找到热点，请靠近后重新扫描";
      case 212: return "信号太弱，请靠近热点";
      case 202: return "认证失败，请检查密码";
      case 203: return "热点拒绝连接";
      case 15: case 204: return "握手超时，请检查密码和信号";
      case 210: case 211: return "热点加密方式不兼容";
      default: return "连接失败，请重试";
    }
  }
  if (wifi->connected[0]) return "已连接";
  if (settings->screen == PET_SETTINGS_WIFI_SAVED && !wifi->saved_count) return "没有已保存网络";
  if (settings->screen == PET_SETTINGS_WIFI && !wifi->count) return "未找到网络，请重新扫描";
  return "请选择网络";
}

static void move_wifi_page(bool next, bool wrap) {
  unsigned start = page.list_start;
  if (next) {
    start += PET_SETTINGS_WIFI_ROWS;
    if (start >= wifi_list_count()) {
      if (!wrap) return;
      start = 0;
    }
  } else {
    if (!start) return;
    start -= PET_SETTINGS_WIFI_ROWS;
  }
  page.selected = start;
  page.list_start = start;
}

static void request(pet_wifi_operation_t operation) {
  page.request_busy = !pet_device_wifi_request(operation, &page.target, password);
}

static void open_wifi_list(pet_settings_screen_t screen) {
  page.screen = screen;
  page.selected = page.list_start = 0;
  page.unsupported = page.request_busy = false;
}

static void select_network(void) {
  const unsigned count = wifi_list_count();
  if (page.selected >= count) {
    if (page.screen == PET_SETTINGS_WIFI && page.selected == count) {
      if (!page.wifi.request_pending) request(PET_WIFI_SCAN);
    } else open_wifi_list(page.screen == PET_SETTINGS_WIFI ? PET_SETTINGS_WIFI_SAVED : PET_SETTINGS_WIFI);
    return;
  }
  network_parent = page.screen;
  page.password_edit = false;
  if (page.screen == PET_SETTINGS_WIFI_SAVED) {
    const pet_wifi_saved_network_t *entry = &page.wifi.saved[page.selected];
    page.target = (pet_wifi_ap_t){.security = entry->security};
    memcpy(page.target.ssid, entry->ssid, sizeof(page.target.ssid));
    page.saved_target = (int)page.selected;
    page.selected = 0;
    page.screen = PET_SETTINGS_WIFI_NETWORK;
    return;
  }
  if (page.wifi.request_pending) return;
  page.target = page.wifi.aps[page.selected];
  page.unsupported = page.target.security == PET_WIFI_UNSUPPORTED;
  if (page.unsupported) return;
  for (unsigned i = 0; i < page.wifi.saved_count; ++i) {
    if (page.target.security == page.wifi.saved[i].security && !strcmp(page.target.ssid,page.wifi.saved[i].ssid)) {
      page.saved_target = (int)i;
      page.selected = 0;
      page.screen = PET_SETTINGS_WIFI_NETWORK;
      return;
    }
  }
  clear_password();
  if (page.target.security == PET_WIFI_OPEN) {
    request(PET_WIFI_CONNECT);
    return;
  }
  page.short_password = false;
  page.keyboard = page.key_row = page.key_column = 0;
  page.screen = PET_SETTINGS_PASSWORD;
}

static void wifi_network_input(pet_settings_input_t input) {
  const unsigned limit = page.screen == PET_SETTINGS_WIFI_FORGET ? 1 : 2 + (page.target.security != PET_WIFI_OPEN);
  if (input == PET_SETTINGS_UP && page.selected) --page.selected;
  if (input == PET_SETTINGS_DOWN && page.selected < limit) ++page.selected;
  if (input != PET_SETTINGS_ENTER || page.wifi.request_pending) return;
  if (page.screen == PET_SETTINGS_WIFI_FORGET && page.selected == 0) {
    page.screen = PET_SETTINGS_WIFI_NETWORK;
    page.selected = 2;
    return;
  }
  if (page.saved_target < 0) return;
  if (page.screen == PET_SETTINGS_WIFI_FORGET) {
    request(PET_WIFI_FORGET);
    if (!page.request_busy) open_wifi_list(PET_SETTINGS_WIFI_SAVED);
  } else if (page.selected == 0) {
    request(PET_WIFI_CONNECT_SAVED);
  } else if (page.selected == 1) {
    request(page.wifi.saved[page.saved_target].auto_join ? PET_WIFI_AUTO_OFF : PET_WIFI_AUTO_ON);
  } else if (page.selected == 3) {
    clear_password();
    page.password_edit = true;
    page.short_password = page.request_busy = false;
    page.keyboard = page.key_row = page.key_column = 0;
    page.screen = PET_SETTINGS_PASSWORD;
  } else {
    page.screen = PET_SETTINGS_WIFI_FORGET;
    page.selected = 0;
  }
}

unsigned pet_settings_key_columns(unsigned row) {
  return row == 4 ? 5 : 10;
}

const char *pet_settings_key_label(unsigned keyboard, unsigned row, unsigned column, char ascii[2]) {
  static const char *const lower[] = {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_@."};
  static const char *const upper[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM_@."};
  static const char *const symbols[] = {"!#$%&*+=/\\", "[]{}()<>:;", "'\"?`~^|,._", "@-01234567"};
  if (row == 4) {
    static const char *const buttons[] = {"大小写", "符号", "空格", "删除", "连接"};
    return buttons[column];
  }
  const char *const *keys = keyboard == 2 ? symbols : keyboard == 1 ? upper : lower;
  ascii[0] = keys[row][column];
  ascii[1] = 0;
  return ascii;
}

static bool feed_apply(bool forget) {
  uint32_t epoch = pet_private_storage_epoch();
  pet_private_feed_t current;
  pet_wifi_endpoint_t endpoint;
  if (!pet_p4_miniapp_private_feed(&current, &endpoint) ||
      memcmp(current.id, selected_feed.id, sizeof(current.id)) ||
      !pet_private_secret_schedule_at(&selected_feed, &feed_secret, forget, epoch)) return false;
#if PET_P4_SCOPED_WRITE
  page.write_submitted = false;
#endif
  page.feed_submitted = true;
  mbedtls_platform_zeroize(&feed_secret, sizeof(feed_secret));
  return true;
}

static void activate_key(void) {
  page.entry_full = page.short_password = false;
  if (page.key_row == 4) {
    if (page.key_column == 0) {
      page.keyboard = page.keyboard == 0 ? 1 : 0;
      return;
    }
    if (page.key_column == 1) {
      page.keyboard = page.keyboard == 2 ? 0 : 2;
      return;
    }
    if (page.key_column == 3) {
      if (page.password_length) password[--page.password_length] = 0;
      return;
    }
    if (page.key_column == 4) {
      if (page.screen == PET_SETTINGS_FEED_ENTRY) {
        if (!page.password_length && page.feed_kind != PET_PRIVATE_MQTT) {
          page.short_password = true;
          return;
        }
        if (page.feed_username) {
          memcpy(feed_secret.user, password, page.password_length + 1);
          clear_password();
          page.feed_username = false;
        } else {
          memcpy(feed_secret.token, password, page.password_length + 1);
          if (feed_apply(false)) {
            clear_password();
            page.screen = PET_SETTINGS_FEED;
            page.selected = 0;
          } else {
            mbedtls_platform_zeroize(feed_secret.token, sizeof(feed_secret.token));
            page.short_password = true;
          }
        }
        return;
      }
      if (page.password_length < 8) {
        page.short_password = true;
        return;
      }
      request(PET_WIFI_CONNECT);
      if (!page.request_busy) {
        clear_password();
        page.screen = PET_SETTINGS_WIFI;
      }
      return;
    }
  }
  const unsigned limit = page.screen != PET_SETTINGS_FEED_ENTRY ? PET_WIFI_PASSWORD_SIZE - 2 :
    page.feed_username ? PET_PRIVATE_USER_MAX : PET_PRIVATE_TOKEN_MAX;
  if (page.password_length >= limit) {
    page.entry_full = true;
    return;
  }
  char ascii[2];
  const char key = page.key_row == 4 ? ' ' :
    pet_settings_key_label(page.keyboard, page.key_row, page.key_column, ascii)[0];
  password[page.password_length++] = key;
  password[page.password_length] = 0;
}

static void timezone_input(pet_settings_input_t input) {
  if (input == PET_SETTINGS_ENTER) {
    pet_wifi_clock_set_timezone(page.timezone_draft);
    page.screen = PET_SETTINGS_HOME;
    page.selected = 2;
    return;
  }
  if (input == PET_SETTINGS_UP || input == PET_SETTINGS_DOWN) {
    page.timezone_draft = page.timezone_draft == PET_WIFI_TZ_AUTO
      ? pet_wifi_clock_auto_offset() : PET_WIFI_TZ_AUTO;
  } else if (input == PET_SETTINGS_LEFT || input == PET_SETTINGS_RIGHT) {
    int32_t minutes = page.timezone_draft == PET_WIFI_TZ_AUTO
      ? pet_wifi_clock_auto_offset() : page.timezone_draft;
    minutes += input == PET_SETTINGS_LEFT ? -15 : 15;
    if (minutes < PET_WIFI_TZ_MIN) minutes = PET_WIFI_TZ_MIN;
    if (minutes > PET_WIFI_TZ_MAX) minutes = PET_WIFI_TZ_MAX;
    page.timezone_draft = minutes;
  }
}

static void community_input(pet_settings_input_t input) {
  pet_community_view_t v;
  pet_community_view(&v);
  if (v.busy) return;
  if (page.community_confirm) {
    if (input==PET_SETTINGS_LEFT || input==PET_SETTINGS_RIGHT || input==PET_SETTINGS_UP || input==PET_SETTINGS_DOWN)
      page.selected=!page.selected;
    if (input==PET_SETTINGS_ENTER) {
      if (page.selected) pet_community_action(page.community_delete);
      page.community_confirm=false;
      page.selected=2;
    }
    return;
  }
  if (input==PET_SETTINGS_UP && page.selected>=2) page.selected-=2;
  if (input==PET_SETTINGS_DOWN && page.selected<4) page.selected+=2;
  if (input==PET_SETTINGS_LEFT || input==PET_SETTINGS_RIGHT) page.selected^=1;
  if (input!=PET_SETTINGS_ENTER) return;
  switch (page.selected) {
    case 0: pet_community_move(-1); break;
    case 1: pet_community_move(1); break;
    case 2: case 3:
      if (page.selected==2 ? v.install : v.remove) {
        page.community_delete=page.selected==3;
        page.community_confirm=true;
        page.selected=0;
      }
      break;
    case 4: if (v.installed) pet_community_browse(false); else pet_community_local(); break;
    case 5: if (v.installed) pet_community_local(); else pet_community_browse(false); break;
  }
}

void pet_settings_input(pet_p4_runtime_state_t *state, pet_settings_input_t input,
                        unsigned long long ts_ms) {
  if (!pet_settings_active(state) || state->approval_active) return;
  update_list();
  if (page.confirm_remove) {
    if (input == PET_SETTINGS_BACK || input == PET_SETTINGS_ENTER) {
      if (input == PET_SETTINGS_ENTER && page.selected) {
        if (page.screen == PET_SETTINGS_LAN) {
          pet_lan_pair_view_t v;
          pet_p4_lan_pair_view(&v);
          if (v.paired && !v.busy) pet_p4_lan_forget();
        } else if (page.screen == PET_SETTINGS_BLE) {
          if (pet_ble_shortcut_paired()) pet_ble_shortcut_forget();
        } else if (page.screen == PET_SETTINGS_FEED) {
          page.request_busy = !feed_apply(true);
        }
      }
      page.confirm_remove = false;
      page.selected = page.screen != PET_SETTINGS_LAN;
    } else page.selected = input == PET_SETTINGS_RIGHT || input == PET_SETTINGS_DOWN;
    state->last_update_ms = ts_ms;
    return;
  }
  if (input == PET_SETTINGS_BACK) {
    clear_password();
    mbedtls_platform_zeroize(&feed_secret, sizeof(feed_secret));
    if (page.screen == PET_SETTINGS_COMMUNITY) {
      if (page.community_confirm) { page.community_confirm=false; page.selected=2; }
      else { pet_community_close(); page.screen=PET_SETTINGS_HOME; page.selected=4; }
      state->last_update_ms=ts_ms;
      return;
    }
    if (page.screen == PET_SETTINGS_FEED_ENTRY) {
      page.screen = PET_SETTINGS_FEED;
      page.selected = 0;
    } else if (page.screen == PET_SETTINGS_FEED) {
#if PET_P4_SCOPED_WRITE
      pet_device_write_cancel();
#endif
      snprintf(state->screen_page, sizeof(state->screen_page), "%s", return_page);
    } else if (page.screen == PET_SETTINGS_PASSWORD) {
      page.screen = page.password_edit ? PET_SETTINGS_WIFI_NETWORK : PET_SETTINGS_WIFI;
    } else if (page.screen == PET_SETTINGS_AV) {
      page.av_pending = false;
      page.screen = PET_SETTINGS_HOME;
      page.selected = 5;
    }
    else if (page.screen == PET_SETTINGS_LAN || page.screen == PET_SETTINGS_TIMEZONE) {
      page.selected = page.screen == PET_SETTINGS_LAN ? 1 : 2;
      page.screen = PET_SETTINGS_HOME;
    }
    else if (page.screen == PET_SETTINGS_BLE) {
      pet_ble_shortcut_close_pair();
      page.screen = PET_SETTINGS_HOME;
      page.selected = 3;
    } else if (page.screen == PET_SETTINGS_WIFI_FORGET) {
      page.screen = PET_SETTINGS_WIFI_NETWORK;
      page.selected = 2;
    } else if (page.screen == PET_SETTINGS_WIFI_NETWORK) {
      open_wifi_list(network_parent);
    } else if (page.screen == PET_SETTINGS_WIFI_SAVED) {
      open_wifi_list(PET_SETTINGS_WIFI);
    } else if (page.screen == PET_SETTINGS_WIFI) {
      pet_device_wifi_cancel();
      page.screen = PET_SETTINGS_HOME;
      page.selected = 0;
    } else snprintf(state->screen_page, sizeof(state->screen_page), "%s", return_page);
  } else if (page.screen == PET_SETTINGS_HOME) {
    if ((input == PET_SETTINGS_UP || input == PET_SETTINGS_LEFT) && page.selected > 0) --page.selected;
    if ((input == PET_SETTINGS_DOWN || input == PET_SETTINGS_RIGHT) && page.selected < 5u) ++page.selected;
    if (input == PET_SETTINGS_ENTER) {
      if (page.selected == 0) {
        open_wifi_list(PET_SETTINGS_WIFI);
        request(PET_WIFI_SCAN);
      } else if (page.selected == 1) {
        page.screen = PET_SETTINGS_LAN;
        page.selected = 0;

      } else if (page.selected == 5) {
        page.screen = PET_SETTINGS_AV;
        page.volume_storage = PET_AV_UNCONFIRMED;
        page.brightness_saved = false;
        page.selected = 0;
        page.volume = applied_volume = page.brightness = applied_brightness = -1;
        request_av(false);
      } else if (page.selected == 4) {
        page.screen=PET_SETTINGS_COMMUNITY;
        page.selected=0;
        page.community_confirm=false;
        pet_community_browse(false);
      } else if (page.selected == 3) {
        page.screen = PET_SETTINGS_BLE;
        page.selected = 0;
        request(PET_WIFI_START);
      } else {
        page.screen = PET_SETTINGS_TIMEZONE;
        page.timezone_draft = pet_wifi_clock_timezone();
      }
    }
  } else if (page.screen == PET_SETTINGS_AV) {
    av_input(input);
  } else if (page.screen == PET_SETTINGS_COMMUNITY) {
    community_input(input);
  } else if (page.screen == PET_SETTINGS_FEED) {
    const unsigned count = pet_settings_feed_actions(&page);
    if (input == PET_SETTINGS_UP) page.selected = (page.selected + count - 1) % count;
    if (input == PET_SETTINGS_DOWN) page.selected = (page.selected + 1) % count;
    if (input == PET_SETTINGS_ENTER && page.feed_can_save &&
        pet_private_local_result() != PET_PRIVATE_BUSY) {
#if PET_P4_SCOPED_WRITE
      if (page.selected == 2) {
        if (page.write_submitted && pet_device_write_result() == PET_PRIVATE_BUSY) return;
        page.write_submitted = pet_device_write_start(&selected_feed);
        page.request_busy = !page.write_submitted;
        page.feed_submitted = false;
      } else
#endif
      if (page.selected == 1) {
#if PET_P4_SCOPED_WRITE
        pet_device_write_cancel();
#endif
        page.confirm_remove = true;
        page.selected = 0;
      } else {
        clear_password();
        mbedtls_platform_zeroize(&feed_secret, sizeof(feed_secret));
        page.feed_username = page.feed_kind == PET_PRIVATE_MQTT || page.feed_kind == PET_MQTT_PUBLISH;
        page.short_password = false;
        page.request_busy = false;
        page.keyboard = page.key_row = page.key_column = 0;
        page.screen = PET_SETTINGS_FEED_ENTRY;
      }
    }
  } else if (page.screen == PET_SETTINGS_BLE) {
    if (input == PET_SETTINGS_LEFT || input == PET_SETTINGS_UP) page.selected = 0;
    if (input == PET_SETTINGS_RIGHT || input == PET_SETTINGS_DOWN) page.selected = 1;
    if (input == PET_SETTINGS_ENTER && pet_ble_shortcut_available() > 0) {
      if (!page.selected) page.request_busy = !pet_ble_keyboard_set_mode(!pet_ble_keyboard_enabled());
      else if (pet_ble_shortcut_paired()) {
        page.confirm_remove = true;
        page.selected = 0;
      } else if (pet_ble_keyboard_enabled()) {
        if (pet_ble_shortcut_pairing()) pet_ble_shortcut_close_pair();
        else pet_ble_shortcut_open_pair();
      }
    }
  } else if (page.screen == PET_SETTINGS_TIMEZONE) {
    timezone_input(input);
  } else if (page.screen == PET_SETTINGS_LAN) {
    page.selected = 0;
    if (input == PET_SETTINGS_ENTER) {
      pet_lan_pair_view_t v;
      pet_p4_lan_pair_view(&v);
      if (v.paired && !v.busy) page.confirm_remove = true;
    }
  } else if (page.screen == PET_SETTINGS_WIFI_NETWORK || page.screen == PET_SETTINGS_WIFI_FORGET) {
    wifi_network_input(input);
  } else if (page.screen == PET_SETTINGS_WIFI || page.screen == PET_SETTINGS_WIFI_SAVED) {
    page.unsupported = false;
    if (input == PET_SETTINGS_UP && page.selected) --page.selected;
    if (input == PET_SETTINGS_DOWN && page.selected < wifi_list_count() + (page.screen == PET_SETTINGS_WIFI)) ++page.selected;
    if (input == PET_SETTINGS_LEFT) move_wifi_page(false, false);
    if (input == PET_SETTINGS_RIGHT) move_wifi_page(true, false);
    if (input == PET_SETTINGS_ENTER) select_network();
    if (input != PET_SETTINGS_ENTER) update_list();
  } else {
    unsigned columns = pet_settings_key_columns(page.key_row);
    if (input == PET_SETTINGS_ENTER) activate_key();
    if (input == PET_SETTINGS_LEFT && page.key_column) --page.key_column;
    if (input == PET_SETTINGS_RIGHT && page.key_column + 1 < columns) ++page.key_column;
    if (input == PET_SETTINGS_UP && page.key_row) --page.key_row;
    if (input == PET_SETTINGS_DOWN && page.key_row + 1 < PET_SETTINGS_KEY_ROWS) ++page.key_row;
    const unsigned next_columns = pet_settings_key_columns(page.key_row);
    if (next_columns != columns) page.key_column = page.key_column * next_columns / columns;
  }
  state->last_update_ms = ts_ms;
}

void pet_settings_wifi_swipe(pet_p4_runtime_state_t *state, int start_x, int start_y,
                             bool next, unsigned long long ts_ms) {
  if (!pet_settings_active(state) || state->approval_active ||
      (page.screen != PET_SETTINGS_WIFI && page.screen != PET_SETTINGS_WIFI_SAVED) ||
      start_x < 24 || start_x >= 616 || start_y < 126 || start_y >= 382) return;
  pet_settings_input(state, next ? PET_SETTINGS_RIGHT : PET_SETTINGS_LEFT, ts_ms);
}

void pet_settings_touch(pet_p4_runtime_state_t *state, int x, int y, unsigned long long ts_ms) {
  if (!pet_settings_active(state) || state->approval_active) return;
  if (x >= 16 && x < 125 && y >= 9 && y < 73) {
    pet_settings_input(state, PET_SETTINGS_BACK, ts_ms);
    return;
  }
  update_list();
  if (page.confirm_remove) {
    if (y < 392 || y >= 452 || x < 24 || x >= 616 || (x >= 312 && x < 328)) return;
    page.selected = x >= 328;
    pet_settings_input(state,PET_SETTINGS_ENTER,ts_ms);
  } else if (page.screen == PET_SETTINGS_HOME) {
    const unsigned step = pet_settings_home_step(&page);
    const unsigned count = 5;
    if (x >= 24 && x < 616 && y >= 103 && (unsigned)(y - 103) < count * step &&
        (unsigned)(y - 103) % step < step - 16) {
      page.selected = (unsigned)(y - 103) / step;
      if (page.selected == 4) {
        if (x >= 312 && x < 328) return;
        if (x >= 328) page.selected = 5;
      }
      pet_settings_input(state, PET_SETTINGS_ENTER, ts_ms);
    }
  } else if (page.screen == PET_SETTINGS_AV) {
    if (page.av_pending || x < 24 || x >= 616) return;
    if (y >= 130 && y < 290 && (y-130)%80 < 60 && (x < 130 || x >= 510)) {
      page.selected = (unsigned)(y-130)/80;
      pet_settings_input(state,x<130?PET_SETTINGS_LEFT:PET_SETTINGS_RIGHT,ts_ms);
    } else if (y >= 392 && y < 452 && (x < 312 || x >= 328)) {
      page.selected = x < 312 ? 2 : 3;
      pet_settings_input(state,PET_SETTINGS_ENTER,ts_ms);
    }
  } else if (page.screen==PET_SETTINGS_COMMUNITY) {
    if (y<241 || y>=451 || (y-241)%70>=60 || x<24 || x>=616 || (x>=312 && x<328)) return;
    unsigned index=(unsigned)(y-241)/70*2+(x>=328);
    if (page.community_confirm && index>1) return;
    page.selected=index;
    pet_settings_input(state,PET_SETTINGS_ENTER,ts_ms);
  } else if (page.screen == PET_SETTINGS_FEED) {
    if (x >= 24 && x < 616 && y >= 280 && y < 280 + (int)pet_settings_feed_actions(&page) * 70 && (y - 280) % 70 < 58) {
      page.selected = (y - 280) / 70;
      pet_settings_input(state, PET_SETTINGS_ENTER, ts_ms);
    }
  } else if (page.screen == PET_SETTINGS_BLE) {
    if (x >= 24 && x < 616 && (x < 312 || x >= 328) && y >= 392 && y < 452) {
      page.selected = x >= 328;
      pet_settings_input(state,PET_SETTINGS_ENTER,ts_ms);
    }
  } else if (page.screen == PET_SETTINGS_TIMEZONE) {
    if (y >= 160 && y < 232 && x >= 24 && x < 130)
      pet_settings_input(state, PET_SETTINGS_LEFT, ts_ms);
    else if (y >= 160 && y < 232 && x >= 510 && x < 616)
      pet_settings_input(state, PET_SETTINGS_RIGHT, ts_ms);
    else if (x >= 24 && x < 616 && y >= 260 && y < 330)
      pet_settings_input(state, PET_SETTINGS_UP, ts_ms);
    else if (x >= 24 && x < 616 && y >= 392 && y < 452)
      pet_settings_input(state, PET_SETTINGS_ENTER, ts_ms);
  } else if (page.screen == PET_SETTINGS_LAN) {
    if (x < 24 || x >= 616) return;
    if (y >= 392 && y < 452) {
      pet_settings_input(state, PET_SETTINGS_ENTER, ts_ms);
    }
  } else if (page.screen == PET_SETTINGS_WIFI_NETWORK || page.screen == PET_SETTINGS_WIFI_FORGET) {
    if (x < 24 || x >= 616 || y < PET_SETTINGS_WIFI_ACTION_TOP) return;
    const unsigned offset = y - PET_SETTINGS_WIFI_ACTION_TOP;
    const unsigned row = offset / PET_SETTINGS_WIFI_ACTION_STEP;
    const unsigned count = page.screen == PET_SETTINGS_WIFI_NETWORK ? 3 + (page.target.security != PET_WIFI_OPEN) : 2;
    if (row < count && offset % PET_SETTINGS_WIFI_ACTION_STEP < PET_SETTINGS_WIFI_ACTION_HEIGHT) {
      page.selected = row;
      wifi_network_input(PET_SETTINGS_ENTER);
      state->last_update_ms = ts_ms;
    }
  } else if (page.screen == PET_SETTINGS_WIFI || page.screen == PET_SETTINGS_WIFI_SAVED) {
    const unsigned count = wifi_list_count();
    const bool saved_list = page.screen == PET_SETTINGS_WIFI_SAVED;
    if (x >= 24 && x < 616 && y >= 126 && y < 382 && (y - 126) % 64 < 58) {
      unsigned index = page.list_start + (unsigned)(y - 126) / 64;
      if (index >= count) return;
      page.selected = index;
    } else if (y >= 395 && y < 455) {
      if (x >= 24 && x < 210) page.selected = count;
      else if (x >= 230 && x < 415 && !saved_list) page.selected = count + 1;
      else if (x >= 435 && x < 616 && count > PET_SETTINGS_WIFI_ROWS) {
        move_wifi_page(true, true);
        state->last_update_ms = ts_ms;
        return;
      } else return;
    } else return;
    page.unsupported = false;
    select_network();
    state->last_update_ms = ts_ms;
  } else if ((page.screen == PET_SETTINGS_PASSWORD || page.screen == PET_SETTINGS_FEED_ENTRY) &&
             x >= PET_SETTINGS_KEY_LEFT && x < PET_SETTINGS_KEY_LEFT + PET_SETTINGS_KEY_WIDTH &&
             y >= PET_SETTINGS_KEY_TOP && y < PET_SETTINGS_KEY_TOP + PET_SETTINGS_KEY_ROWS * PET_SETTINGS_KEY_ROW_HEIGHT) {
    unsigned row = (unsigned)(y - PET_SETTINGS_KEY_TOP) / PET_SETTINGS_KEY_ROW_HEIGHT;
    unsigned key_width = PET_SETTINGS_KEY_WIDTH / pet_settings_key_columns(row);
    unsigned offset = (unsigned)(x - PET_SETTINGS_KEY_LEFT) % key_width;
    if (offset < PET_SETTINGS_KEY_GAP / 2 || offset >= key_width - PET_SETTINGS_KEY_GAP + PET_SETTINGS_KEY_GAP/2 ||
        (y-PET_SETTINGS_KEY_TOP)%PET_SETTINGS_KEY_ROW_HEIGHT >= PET_SETTINGS_KEY_ROW_HEIGHT-PET_SETTINGS_KEY_GAP) return;
    page.key_row = row;
    page.key_column = (unsigned)(x - PET_SETTINGS_KEY_LEFT) / key_width;
    pet_settings_input(state, PET_SETTINGS_ENTER, ts_ms);
  }
}

void pet_settings_maintain(pet_p4_runtime_state_t *state, bool interrupted) {
  static bool prompted;
  if (!pet_settings_active(state) && !strcmp(state->screen_page, "app") && !interrupted) {
    if (!pet_wifi_data_authorization_needed(NULL)) prompted = false;
    else if (!prompted) {
      pet_private_feed_t binding;
      pet_wifi_endpoint_t endpoint;
      if (pet_p4_miniapp_private_feed(&binding, &endpoint) &&
          pet_wifi_data_authorization_needed(&binding))
        prompted = pet_settings_open(state, state->last_update_ms + 1);
    }
  } else if (!pet_settings_active(state)) prompted = false;
  if (page_open && !interrupted && pet_settings_active(state) &&
      (page.screen == PET_SETTINGS_FEED || page.screen == PET_SETTINGS_FEED_ENTRY)) {
    pet_private_feed_t current;
    interrupted = !pet_p4_miniapp_private_feed(&current,NULL) ||
                  memcmp(current.id,selected_feed.id,sizeof(current.id)) != 0;
  }
  if (!page_open || (pet_settings_active(state) && !interrupted)) return;
  if (page.screen==PET_SETTINGS_COMMUNITY) pet_community_close();
  if (pet_settings_active(state))
    snprintf(state->screen_page, sizeof(state->screen_page), "%s", return_page);
  clear_password();
  mbedtls_platform_zeroize(&feed_secret, sizeof(feed_secret));
  pet_device_wifi_cancel();
  pet_ble_shortcut_close_pair();
#if PET_P4_SCOPED_WRITE
  pet_device_write_cancel();
#endif
  page_open = false;
}

void pet_settings_snapshot(pet_settings_view_t *view) {
  update_list();
  *view = page;
  if (view->screen==PET_SETTINGS_COMMUNITY) pet_community_view(&view->community);
  if (view->screen == PET_SETTINGS_FEED && view->feed_submitted)
    view->feed_result = pet_private_local_result();
#if PET_P4_SCOPED_WRITE
  if (view->screen == PET_SETTINGS_FEED && view->write_submitted)
    view->feed_result = pet_device_write_result();
#endif
  if (view->screen == PET_SETTINGS_BLE) {
    view->ble_enabled = pet_ble_keyboard_enabled();
    view->ble_connected = pet_ble_keyboard_connected();
    view->ble_available = pet_ble_shortcut_available();
    view->ble_paired = pet_ble_shortcut_paired();
    view->ble_pairing = pet_ble_shortcut_pairing();
    view->ble_code = pet_ble_shortcut_code();
  }
  if (view->screen == PET_SETTINGS_TIMEZONE)
    view->timezone_effective = pet_wifi_clock_auto_offset();
  if (view->screen == PET_SETTINGS_LAN) {
    pet_p4_lan_pair_view(&view->lan);
  }
}
#endif
