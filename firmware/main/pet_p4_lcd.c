/*
 * [Input] Waveshare panel/BSP operations, boot lifecycle, and RGB565 pixels.
 * [Output] Backlight-gated LCD initialization, first-frame reveal, and draws.
 * [Pos] ESP32-P4 LCD hardware boundary.
 * [Sync] If this file changes, update `firmware/.folder.md`.
 */

#include "pet_p4_lcd.h"
#if PET_P4_RENDER_LCD_SPLIT_PROBE
#include "esp_timer.h"
#endif
#include <string.h>

#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_4_3.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#if BSP_LCD_PANEL_AUTO
#include "driver/ppa.h"
#include "pet_p4_nv3049_pixels.h"
#include "pet_p4_touch.h"
#endif
#if BSP_LCD_PANEL_AUTO || PET_P4_SYSTEM_WIFI
#include "nvs.h"
#endif

static const char *TAG = "pet-p4-lcd";
static const int LCD_BACKLIGHT_FORCE_LEVEL = 1;
static const TickType_t LCD_REFRESH_WAIT_TICKS = pdMS_TO_TICKS(250);

static esp_lcd_panel_handle_t g_panel_handle;
static esp_lcd_panel_io_handle_t g_lcd_io;
static SemaphoreHandle_t g_refresh_done_sem;
static esp_err_t g_backlight_status = ESP_ERR_INVALID_STATE;
static bool g_backlight_revealed;
static bool g_forced_backlight_logged;
static bool g_refresh_callback_logged;

#if BSP_LCD_PANEL_AUTO
static _Atomic int g_panel_model = PET_LCD_PANEL_UNKNOWN;
static atomic_bool g_no_touch_profile;
static const char *LCD_NVS_NAMESPACE = "pet_lcd";
static const char *LCD_NVS_PROFILE = "nv3049_no_tp";

static esp_err_t load_no_touch_profile(bool *enabled) {
  nvs_handle_t handle;
  uint8_t value = 0;
  esp_err_t err = nvs_open(LCD_NVS_NAMESPACE, NVS_READONLY, &handle);
  if (err == ESP_OK) {
    err = nvs_get_u8(handle, LCD_NVS_PROFILE, &value);
    nvs_close(handle);
  }
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
  if (value > 1) return ESP_ERR_INVALID_STATE;
  *enabled = value == 1;
  return ESP_OK;
}

esp_err_t pet_p4_lcd_save_no_touch_profile(bool enabled) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(LCD_NVS_NAMESPACE, NVS_READWRITE, &handle);
  if (err != ESP_OK) return err;
  err = nvs_set_u8(handle, LCD_NVS_PROFILE, enabled ? 1 : 0);
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  return err;
}

bool pet_p4_lcd_no_touch_profile(void) {
  return atomic_load_explicit(&g_no_touch_profile, memory_order_relaxed);
}

static uint8_t *g_nv_buffers[2];
static unsigned g_nv_front;
static atomic_uint g_nv_refresh;
static TaskHandle_t g_nv_render_task;
// CPU0 render-task state. Only a successful publication followed by a DMA
// refresh on that core proves which of the two buffers is safe to overwrite.
static enum { NV_READY, NV_FIRST_LATCH, NV_WAIT_LATCH, NV_UNKNOWN } g_nv_state;
static unsigned g_nv_published_at, g_nv_published_front;
static unsigned g_nv_recoveries;
static bool g_nv_recovery_redraw;
static TickType_t g_nv_last_draw_tick, g_nv_max_recovery_gap;
static bool g_nv_reveal_on_full_draw;
static ppa_client_handle_t g_nv_ppa;
static atomic_bool g_nv_ppa_active;
static uint32_t g_nv_last_refresh_tick;
static atomic_uint g_nv_max_refresh_gap;

bool pet_p4_lcd_nv3049_ppa_active(void) {
  return atomic_load_explicit(&g_nv_ppa_active, memory_order_relaxed);
}

unsigned pet_p4_lcd_nv3049_max_refresh_gap(void) {
  return atomic_load_explicit(&g_nv_max_refresh_gap, memory_order_relaxed);
}

static esp_err_t nv_convert_ppa(const uint16_t *pixels, unsigned width, unsigned height, uint8_t *out) {
  const ppa_srm_oper_config_t config = {
    .in = {
      .buffer = pixels, .pic_w = width, .pic_h = height,
      .block_w = width, .block_h = height, .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
    },
    .out = {
      .buffer = out, .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES * 3U,
      .pic_w = BSP_LCD_H_RES, .pic_h = BSP_LCD_V_RES, .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
    },
    .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
    .scale_x = 1.0f, .scale_y = 1.0f, .mode = PPA_TRANS_MODE_BLOCKING,
  };
  return ppa_do_scale_rotate_mirror(g_nv_ppa, &config);
}

static void nv_init_conversion(void) {
  const ppa_client_config_t config = {
    .oper_type = PPA_OPERATION_SRM,
    .data_burst_length = PPA_DATA_BURST_LENGTH_32,
  };
  if (ppa_register_client(&config, &g_nv_ppa) != ESP_OK) return;

  // Check RGB byte order and truncation on this silicon in the offscreen buffer.
  static const uint16_t colors[] = {
    0xf800, 0x07e0, 0x001f, 0x0000, 0xffff, 0x616b, 0xe6c5, 0x2ad6,
    0x001f, 0x07e0, 0xf800, 0x0000, 0xffff, 0x616b, 0xe6c5, 0x2ad6,
  };
  esp_err_t err = nv_convert_ppa(colors, 8, 2, g_nv_buffers[1]);
  if (err == ESP_OK) {
    for (unsigned i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
      const uint8_t *bgr = g_nv_buffers[1] + ((i / 8) * BSP_LCD_H_RES + i % 8) * 3;
      uint16_t packed = ((bgr[2] & 0xf8U) << 8) | ((bgr[1] & 0xfcU) << 3) | (bgr[0] >> 3);
      if (packed != colors[i]) {
        err = ESP_ERR_INVALID_RESPONSE;
        break;
      }
    }
  }
  if (err == ESP_OK) {
    atomic_store_explicit(&g_nv_ppa_active, true, memory_order_relaxed);
  } else {
    ESP_LOGW(TAG, "NV3049 RGB888 PPA self-test failed: %s", esp_err_to_name(err));
    ppa_unregister_client(g_nv_ppa);
    g_nv_ppa = NULL;
  }
}

static esp_err_t nv_wait_for_refresh(unsigned published_at) {
  TickType_t started = xTaskGetTickCount();
  while (atomic_load_explicit(&g_nv_refresh, memory_order_relaxed) == published_at) {
    TickType_t elapsed = xTaskGetTickCount() - started;
    if (elapsed >= LCD_REFRESH_WAIT_TICKS) return ESP_ERR_TIMEOUT;
    // The semaphore is a wakeup hint. Recheck the counter even on timeout.
    (void)xSemaphoreTake(g_refresh_done_sem, LCD_REFRESH_WAIT_TICKS - elapsed);
  }
  return ESP_OK;
}

static esp_err_t nv_publish(unsigned index) {
  esp_err_t err = esp_lcd_panel_draw_bitmap(
    g_panel_handle, 0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES, g_nv_buffers[index]);
  if (err != ESP_OK) {
    // Even an IRQ after this error cannot tell us which index was latched.
    g_nv_state = NV_UNKNOWN;
  } else {
    g_nv_published_front = index;
    g_nv_published_at = atomic_load_explicit(&g_nv_refresh, memory_order_relaxed);
    g_nv_state = NV_WAIT_LATCH;
  }
  return err;
}

static esp_err_t nv_recover(void) {
  if (g_nv_state == NV_READY) return ESP_OK;
  if (g_nv_state == NV_UNKNOWN) {
    // The pinned DPI driver's panel-owned-buffer path only flushes cache
    // and selects an index. Republish the last confirmed image untouched.
    esp_err_t err = nv_publish(g_nv_front);
    if (err != ESP_OK) return err;
    err = nv_wait_for_refresh(g_nv_published_at);
    if (err != ESP_OK) return err;
  } else if (atomic_load_explicit(&g_nv_refresh, memory_order_relaxed) == g_nv_published_at) {
    // Persistent IRQ loss: the pinned DPI APIs expose neither a latched
    // framebuffer index nor a stop-and-confirm-idle operation. Deleting the
    // panel frees both buffers. Keep them allocated and frozen, and poll
    // once per draw until a real DMA callback advances this fence.
    return ESP_ERR_TIMEOUT;
  }
  g_nv_front = g_nv_published_front;
  if (g_nv_state == NV_FIRST_LATCH) nv_init_conversion(); // SDK is scanning fb0 now.
  g_nv_state = NV_READY;
  g_nv_recovery_redraw = true;
  return ESP_OK;
}
#endif

#if PET_P4_SYSTEM_WIFI
static DRAM_ATTR _Atomic uint32_t lcd_phase, lcd_worker_active;
static DRAM_ATTR _Atomic uint32_t lcd_sequence, lcd_count, lcd_last_tick, lcd_last_context;

void IRAM_ATTR pet_p4_lcd_on_underrun(void) {
  /* Single interrupt producer. The reader must run outside the ISR. */
  atomic_fetch_add(&lcd_sequence, 1);
  atomic_store(&lcd_last_tick, xTaskGetTickCountFromISR());
  atomic_store(&lcd_last_context,
    atomic_load(&lcd_phase) | (atomic_load(&lcd_worker_active) << 8));
  atomic_fetch_add(&lcd_count, 1);
  atomic_fetch_add(&lcd_sequence, 1);
}

void pet_p4_lcd_render_phase(enum pet_lcd_phase phase) { atomic_store(&lcd_phase, phase); }
void pet_p4_lcd_video_worker(bool active) { atomic_store(&lcd_worker_active, active); }
uint32_t pet_p4_lcd_underrun_count(void) { return atomic_load(&lcd_count); }

pet_lcd_underrun_t pet_p4_lcd_underrun(void) {
  pet_lcd_underrun_t record;
  uint32_t sequence;
  uint32_t context;
  do {
    sequence = atomic_load(&lcd_sequence);
    record.count = atomic_load(&lcd_count);
    record.last_tick = atomic_load(&lcd_last_tick);
    context = atomic_load(&lcd_last_context);
  } while ((sequence & 1) || sequence != atomic_load(&lcd_sequence));
  record.last_phase = context & 255;
  record.video_worker = (context >> 8) != 0;
  return record;
}
#endif

pet_lcd_panel_model_t pet_p4_lcd_panel_model(void) {
#if BSP_LCD_PANEL_AUTO
  return atomic_load_explicit(&g_panel_model, memory_order_relaxed);
#else
  return PET_LCD_PANEL_ST7701;
#endif
}

static esp_err_t force_lcd_backlight_gpio(int level) {
  gpio_config_t io_conf = {
    .pin_bit_mask = 1ULL << BSP_LCD_BACKLIGHT,
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "configure LCD backlight GPIO");
  ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_BACKLIGHT, level), TAG, "force LCD backlight GPIO");
  if (level == LCD_BACKLIGHT_FORCE_LEVEL && !g_forced_backlight_logged) {
    ESP_LOGI(TAG, "LCD backlight revealed gpio=%d", LCD_BACKLIGHT_FORCE_LEVEL);
    g_forced_backlight_logged = true;
  }
  return ESP_OK;
}

esp_err_t pet_p4_lcd_prepare_boot(void) {
  g_backlight_revealed = false;
  g_backlight_status = force_lcd_backlight_gpio(1 - LCD_BACKLIGHT_FORCE_LEVEL);
  return g_backlight_status;
}

static bool IRAM_ATTR lcd_refresh_done_callback(
  esp_lcd_panel_handle_t panel,
  esp_lcd_dpi_panel_event_data_t *edata,
  void *user_ctx
) {
  (void) panel;
  (void) edata;
  SemaphoreHandle_t sem = (SemaphoreHandle_t) user_ctx;
  BaseType_t task_awake = pdFALSE;
  if (sem) {
    xSemaphoreGiveFromISR(sem, &task_awake);
  }
  return task_awake == pdTRUE;
}

#if BSP_LCD_PANEL_AUTO
static bool IRAM_ATTR nv_refresh_done_callback(
  esp_lcd_panel_handle_t panel,
  esp_lcd_dpi_panel_event_data_t *edata,
  void *user_ctx
) {
  uint32_t tick = xTaskGetTickCountFromISR();
  uint32_t gap = tick - g_nv_last_refresh_tick;
  if (g_nv_last_refresh_tick && gap > atomic_load_explicit(&g_nv_max_refresh_gap, memory_order_relaxed)) {
    atomic_store_explicit(&g_nv_max_refresh_gap, gap, memory_order_relaxed);
  }
  g_nv_last_refresh_tick = tick;
  atomic_fetch_add_explicit(&g_nv_refresh, 1, memory_order_relaxed);
  return lcd_refresh_done_callback(panel, edata, user_ctx);
}
#endif

esp_err_t pet_p4_lcd_init(void) {
  if (g_panel_handle) return ESP_OK;

  bsp_display_config_t config = {0};
#if BSP_LCD_PANEL_AUTO
  bool no_touch = false;
  const esp_err_t profile_err = load_no_touch_profile(&no_touch);
  if (profile_err != ESP_OK) {
    ESP_LOGW(TAG, "LCD profile unavailable: %s", esp_err_to_name(profile_err));
  }
  atomic_store_explicit(&g_no_touch_profile, no_touch, memory_order_relaxed);
  if (no_touch) {
    config.nv3049 = true;
    ESP_LOGI(TAG, "Panel NV3049F selected by saved no-touch profile");
  } else {
    config.nv3049 = pet_p4_touch_try_nv3049();
  }
  if (config.nv3049) {
    ESP_RETURN_ON_FALSE(xPortGetCoreID() == 0, ESP_ERR_INVALID_STATE, TAG,
                        "NV3049 initialization and scanout must share CPU0");
  }
#endif
  ESP_RETURN_ON_ERROR(bsp_display_new(&config, &g_panel_handle, &g_lcd_io), TAG, "initialize display");
#if BSP_LCD_PANEL_AUTO
  atomic_store_explicit(&g_panel_model,
    config.nv3049 ? PET_LCD_PANEL_NV3049 : PET_LCD_PANEL_ST7701, memory_order_relaxed);
#endif
  if (!g_refresh_done_sem) {
    g_refresh_done_sem = xSemaphoreCreateBinary();
    if (!g_refresh_done_sem) return ESP_ERR_NO_MEM;
  }
#if BSP_LCD_NV3049_DMA_REFRESH
  if (config.nv3049) {
    pet_p4_lcd_use_dma_refresh(g_panel_handle);
  }
#endif
  esp_lcd_dpi_panel_event_callbacks_t callbacks = {
    .on_refresh_done = lcd_refresh_done_callback,
  };
#if BSP_LCD_PANEL_AUTO
  if (config.nv3049) callbacks.on_refresh_done = nv_refresh_done_callback;
#endif
  ESP_RETURN_ON_ERROR(
    esp_lcd_dpi_panel_register_event_callbacks(g_panel_handle, &callbacks, g_refresh_done_sem),
    TAG,
    "register LCD refresh callback"
  );
#if BSP_LCD_PANEL_AUTO
  if (config.nv3049) {
    void *first = NULL, *second = NULL;
    ESP_RETURN_ON_ERROR(
      esp_lcd_dpi_panel_get_frame_buffer(g_panel_handle, 2, &first, &second),
      TAG, "get NV3049 RGB888 framebuffers");
    g_nv_buffers[0] = first;
    g_nv_buffers[1] = second;
    g_nv_render_task = xTaskGetCurrentTaskHandle();
    // Both pinned SDKs start scanout from index zero. Keep the boot render
    // path retryable if the first interrupt arrives after initialization.
    g_nv_last_draw_tick = xTaskGetTickCount(); // origin until first completed draw
    g_nv_published_front = 0;
    g_nv_published_at = atomic_load(&g_nv_refresh);
    if (nv_wait_for_refresh(g_nv_published_at) == ESP_OK) {
      nv_init_conversion();
    } else {
      g_nv_state = NV_FIRST_LATCH;
      ESP_LOGW(TAG, "waiting for first NV3049 DMA refresh");
    }
  }
#endif
  if (!g_refresh_callback_logged) {
    ESP_LOGI(TAG, "LCD refresh_done callback registered");
    g_refresh_callback_logged = true;
  }
  ESP_LOGI(
    TAG,
    "LCD pixel format=%s bits=%d",
    config.nv3049 || BSP_LCD_COLOR_FORMAT == ESP_LCD_COLOR_FORMAT_RGB888 ? "RGB888" : "RGB565",
    config.nv3049 ? 24 : BSP_LCD_BITS_PER_PIXEL
  );
  // Component/storage migration may run for several seconds after an OTA.
  // Keep the panel hidden until the renderer has committed the first complete
  // frame so the controller's blue reset surface is never shown to the user.
  g_backlight_status = bsp_display_backlight_off();
  esp_err_t force_err = config.nv3049 ? ESP_OK : force_lcd_backlight_gpio(1 - LCD_BACKLIGHT_FORCE_LEVEL);
  if (g_backlight_status != ESP_OK || force_err != ESP_OK) {
    ESP_LOGW(TAG, "display backlight hide failed: %s", esp_err_to_name(g_backlight_status));
  }
  if (g_backlight_status == ESP_OK && force_err != ESP_OK) {
    g_backlight_status = force_err;
  }
  return ESP_OK;
}

#if PET_P4_SYSTEM_WIFI
static atomic_int panel_brightness = -1;

int pet_p4_lcd_brightness(void) { return atomic_load(&panel_brightness); }

static unsigned load_brightness(void) {
  nvs_handle_t nvs;
  uint8_t value=50;
  if (nvs_open("pet_lcd",NVS_READONLY,&nvs)==ESP_OK) {
    if (nvs_get_u8(nvs,"brightness",&value)!=ESP_OK || value<10 || value>100) value=50;
    nvs_close(nvs);
  }
  return value;
}

bool pet_p4_lcd_save_brightness(int value) {
  const int previous=pet_p4_lcd_brightness();
  if (previous<0 || value<10 || value>100) return false;
  if (previous==value) return true;
  nvs_handle_t nvs;
  if (nvs_open("pet_lcd",NVS_READWRITE,&nvs)!=ESP_OK) return false;
  esp_err_t err=bsp_display_brightness_set(value);
  if (err==ESP_OK) err=nvs_set_u8(nvs,"brightness",(uint8_t)value);
  if (err==ESP_OK) err=nvs_commit(nvs);
  nvs_close(nvs);
  if (err==ESP_OK) atomic_store(&panel_brightness,value);
  else if (bsp_display_brightness_set(previous)!=ESP_OK) atomic_store(&panel_brightness,-1);
  return err==ESP_OK;
}
#endif

esp_err_t pet_p4_lcd_reveal(void) {
  if (!g_panel_handle) return ESP_ERR_INVALID_STATE;
  if (g_backlight_revealed) return g_backlight_status;
  const bool nv3049 = pet_p4_lcd_panel_model() == PET_LCD_PANEL_NV3049;
  int brightness=50;
#if PET_P4_SYSTEM_WIFI
  if (nv3049) brightness=(int)load_brightness();
#endif
  g_backlight_status = nv3049 ? bsp_display_brightness_set(brightness) : bsp_display_backlight_on();
  esp_err_t force_err = nv3049 ? ESP_OK : force_lcd_backlight_gpio(LCD_BACKLIGHT_FORCE_LEVEL);
  if (g_backlight_status == ESP_OK && force_err != ESP_OK) {
    g_backlight_status = force_err;
  }
  if (g_backlight_status == ESP_OK) {
    g_backlight_revealed = true;
#if PET_P4_SYSTEM_WIFI
    if (nv3049) atomic_store(&panel_brightness,brightness);
#endif
  } else {
    ESP_LOGW(TAG, "display first-frame reveal failed: %s", esp_err_to_name(g_backlight_status));
  }
  return g_backlight_status;
}

esp_err_t pet_p4_lcd_backlight_status(void) {
  return g_backlight_status;
}

esp_err_t pet_p4_lcd_keep_awake(void) {
  if (!g_panel_handle) return ESP_ERR_INVALID_STATE;
  if (!g_backlight_revealed) return ESP_ERR_INVALID_STATE;
  return g_backlight_status;
}

static esp_err_t lcd_draw_core(int x,int y,int width,int height,const uint16_t *pixels,pet_lcd_observation_t *out) {
#if PET_P4_RENDER_LCD_SPLIT_PROBE
  if(out)memset(out,0,sizeof(*out));
#else
  (void)out;
#endif
  if (!g_panel_handle) return ESP_ERR_INVALID_STATE;
  if (!pixels || width <= 0 || height <= 0) return ESP_ERR_INVALID_ARG;

#if BSP_LCD_PANEL_AUTO
  const bool nv3049 = pet_p4_lcd_panel_model() == PET_LCD_PANEL_NV3049;
  unsigned next = 0;
  if (nv3049) {
    if (xTaskGetCurrentTaskHandle() != g_nv_render_task ||
        xPortGetCoreID() != 0) return ESP_ERR_INVALID_STATE;
    if (x < 0 || y < 0 || width > BSP_LCD_H_RES || height > BSP_LCD_V_RES ||
        x > BSP_LCD_H_RES - width || y > BSP_LCD_V_RES - height) return ESP_ERR_INVALID_ARG;
    esp_err_t recovery = nv_recover();
    if (recovery != ESP_OK) {
      if (!g_backlight_revealed) g_nv_reveal_on_full_draw = true;
      return recovery;
    }
    next = g_nv_front ^ 1U;
    const bool full = !x && !y && width == BSP_LCD_H_RES && height == BSP_LCD_V_RES;
    if (!full) {
      memcpy(g_nv_buffers[next], g_nv_buffers[g_nv_front], BSP_LCD_H_RES * BSP_LCD_V_RES * 3U);
    }
    bool converted = false;
    if (full && atomic_load_explicit(&g_nv_ppa_active, memory_order_relaxed)) {
      esp_err_t err = nv_convert_ppa(pixels, width, height, g_nv_buffers[next]);
      converted = err == ESP_OK;
      if (!converted) {
        atomic_store_explicit(&g_nv_ppa_active, false, memory_order_relaxed);
        ESP_LOGW(TAG, "NV3049 RGB888 PPA disabled: %s", esp_err_to_name(err));
      }
    }
    if (!converted && !pet_nv3049_blit_rgb565(g_nv_buffers[next], x, y, width, height, pixels)) {
      return ESP_ERR_INVALID_ARG;
    }
  } else
#endif
  if (g_refresh_done_sem) {
    while (xSemaphoreTake(g_refresh_done_sem, 0) == pdTRUE) {
    }
  }
#if PET_P4_RENDER_LCD_SPLIT_PROBE
  int64_t submit_begin=out?esp_timer_get_time():0;
#endif
  esp_err_t draw_err;
#if BSP_LCD_PANEL_AUTO
  if (nv3049) {
    draw_err = nv_publish(next);
    if (draw_err != ESP_OK && !g_backlight_revealed) g_nv_reveal_on_full_draw = true;
  } else
#endif
  draw_err = esp_lcd_panel_draw_bitmap(g_panel_handle, x, y, x + width, y + height, pixels);
#if PET_P4_RENDER_LCD_SPLIT_PROBE
  int64_t submit_end=out?esp_timer_get_time():0;
  if(out){out->valid=true;out->submit_us=pet_lcd_elapsed(submit_begin,submit_end,&out->valid);out->submitted=draw_err==ESP_OK;}
#endif
  if (draw_err != ESP_OK) return draw_err;
  bool timed_out;
#if BSP_LCD_PANEL_AUTO
  if (nv3049) {
    timed_out = nv_wait_for_refresh(g_nv_published_at) != ESP_OK;
    if (!timed_out) {
      g_nv_front = next;
      g_nv_state = NV_READY;
      TickType_t now = xTaskGetTickCount();
      if (g_nv_recovery_redraw) {
        TickType_t gap = now - g_nv_last_draw_tick;
        if (gap > g_nv_max_recovery_gap) g_nv_max_recovery_gap = gap;
        ++g_nv_recoveries;
        ESP_LOGW(TAG, "NV3049 redraw recovered count=%u draw_gap_ticks=%u max_gap_ticks=%u",
          g_nv_recoveries, (unsigned)gap, (unsigned)g_nv_max_recovery_gap);
        g_nv_recovery_redraw = false;
      }
      g_nv_last_draw_tick = now;
    } else if (!g_backlight_revealed) {
      g_nv_reveal_on_full_draw = true;
    }
  } else
#endif
  timed_out = g_refresh_done_sem && xSemaphoreTake(g_refresh_done_sem, LCD_REFRESH_WAIT_TICKS) != pdTRUE;
#if PET_P4_RENDER_LCD_SPLIT_PROBE
  if(out&&g_refresh_done_sem){out->refresh_waited=true;out->wait_us=pet_lcd_elapsed(submit_end,esp_timer_get_time(),&out->valid);}
#endif
  if (timed_out) {
    ESP_LOGW(TAG, "LCD refresh wait timed out");
    return ESP_ERR_TIMEOUT;
  }
#if BSP_LCD_PANEL_AUTO
  if (nv3049 && g_nv_reveal_on_full_draw && width == BSP_LCD_H_RES && height == BSP_LCD_V_RES) {
    esp_err_t err = pet_p4_lcd_reveal();
    if (err != ESP_OK) return err;
    g_nv_reveal_on_full_draw = false;
  }
#endif
  return ESP_OK;
}

esp_err_t pet_p4_lcd_draw_rgb565(int x,int y,int width,int height,const uint16_t *pixels){
  return lcd_draw_core(x,y,width,height,pixels,NULL);
}
#if PET_P4_RENDER_LCD_SPLIT_PROBE
esp_err_t pet_p4_lcd_draw_rgb565_observed(int x,int y,int width,int height,const uint16_t *pixels,pet_lcd_observation_t *out){
  return lcd_draw_core(x,y,width,height,pixels,out);
}
#endif

int pet_p4_lcd_scan_i2c(char *buffer, size_t buffer_size) {
  if (!buffer || buffer_size == 0) return 0;
  buffer[0] = '\0';

  esp_err_t init_err = bsp_i2c_init();
  if (init_err != ESP_OK) {
    snprintf(buffer, buffer_size, "init:%s", esp_err_to_name(init_err));
    return 0;
  }

  i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
  if (!bus) {
    snprintf(buffer, buffer_size, "no-bus");
    return 0;
  }

  int found = 0;
  size_t used = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr += 1) {
    if (i2c_master_probe(bus, addr, 50) != ESP_OK) continue;
    int written = snprintf(
      buffer + used,
      buffer_size - used,
      "%s0x%02X",
      found == 0 ? "" : ",",
      addr
    );
    if (written < 0 || (size_t) written >= buffer_size - used) {
      buffer[buffer_size - 1] = '\0';
      break;
    }
    used += (size_t) written;
    found += 1;
  }

  ESP_LOGI(TAG, "I2C scan found %d device(s): %s", found, found > 0 ? buffer : "none");
  return found;
}
