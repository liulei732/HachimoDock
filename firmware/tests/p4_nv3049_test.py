"""Test the NV3049 scanout and CST328 with portable panel and bus fixtures."""
from hashlib import sha256
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def compile_and_run(binary, sources, includes):
    subprocess.run(
        ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         *['-I' + str(path) for path in includes],
         *[str(ROOT / source) for source in sources], '-o', str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)


def test_rotated_rgb888_and_touch_coordinates(tmp_path):
    compile_and_run(
        tmp_path / 'nv3049-pixels',
        ['tests/nv3049_pixels_test.c', 'main/pet_p4_touch_core.c'],
        [ROOT / 'main'],
    )


def test_cst328_bus_and_reports(tmp_path):
    for header in ('esp_err.h', 'esp_log.h', 'driver/i2c_master.h',
                   'freertos/FreeRTOS.h', 'freertos/task.h'):
        path = tmp_path / 'mock' / header
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "cst328_host_stubs.h"\n')
    compile_and_run(
        tmp_path / 'cst328',
        ['tests/cst328_driver_test.c', 'main/pet_p4_cst328.c'],
        [ROOT / 'main', ROOT / 'tests', tmp_path / 'mock'],
    )


def test_panel_selection_requires_a_verified_controller(tmp_path):
    touch = (ROOT / 'main/pet_p4_touch.c').read_text()
    entry = 'bool pet_p4_touch_try_nv3049(void) {'
    detector = entry + touch.split(entry, 1)[1].split('\n}\n', 1)[0] + '\n}\n'
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>

typedef int esp_err_t;
typedef void *i2c_master_bus_handle_t;
enum { ESP_OK, ESP_ERR_INVALID_STATE, ESP_ERR_NOT_FOUND, ESP_ERR_NOT_SUPPORTED, ESP_ERR_TIMEOUT };
enum { PET_CST328_ADDRESS = 0x1a };
static int g_address, g_cst328_info;
static bool bus_available;
static esp_err_t init_result, probe_result, cst_result;
static unsigned probes, cst_calls, closed;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static int bsp_i2c_init(void) { return init_result; }
static void *bsp_i2c_get_handle(void) { return bus_available ? &g_address : NULL; }
static int i2c_master_probe(void *bus, int address, int timeout) {
    assert(bus == &g_address && address == PET_CST328_ADDRESS && timeout == 20);
    probes++;
    return probe_result;
}
static int pet_cst328_init(void *bus, int *info) {
    assert(bus == &g_address && info == &g_cst328_info);
    cst_calls++;
    return cst_result;
}
static void pet_cst328_deinit(void) { closed++; }
'''
    scenarios = r'''
static void reset(int init, bool bus, int probe, int cst) {
    init_result = init;
    bus_available = bus;
    probe_result = probe;
    cst_result = cst;
    probes = cst_calls = closed = 0;
    g_address = 0;
}
int main(void) {
    reset(ESP_OK, true, ESP_OK, ESP_OK);
    assert(pet_p4_touch_try_nv3049() && g_address == PET_CST328_ADDRESS);
    assert(probes == 1 && cst_calls == 1 && closed == 0);

    // A legacy module without touch still takes the legacy display path.
    reset(ESP_OK, true, ESP_ERR_NOT_FOUND, ESP_OK);
    assert(!pet_p4_touch_try_nv3049() && g_address == 0);
    assert(probes == 1 && cst_calls == 0 && closed == 0);

    reset(ESP_OK, true, ESP_ERR_TIMEOUT, ESP_OK);
    assert(!pet_p4_touch_try_nv3049() && probes == 1 && cst_calls == 0);

    // An I2C address ACK alone cannot select NV3049.
    reset(ESP_OK, true, ESP_OK, ESP_ERR_NOT_SUPPORTED);
    assert(!pet_p4_touch_try_nv3049() && g_address == 0);
    assert(probes == 1 && cst_calls == 1 && closed == 1);

    reset(ESP_ERR_INVALID_STATE, true, ESP_OK, ESP_OK);
    assert(!pet_p4_touch_try_nv3049() && probes == 0 && cst_calls == 0);
    reset(ESP_OK, false, ESP_OK, ESP_OK);
    assert(!pet_p4_touch_try_nv3049() && probes == 0 && cst_calls == 0);
}
'''
    source = tmp_path / 'panel_selection.c'
    source.write_text(harness + detector + scenarios)
    binary = tmp_path / 'panel-selection'
    subprocess.run(
        ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)


def test_nv3049_paths_share_init_trace_and_st7701_is_isolated(tmp_path):
    bsp_dir = ROOT / 'components/esp32_p4_wifi6_touch_lcd_4_3'
    bsp = (bsp_dir / 'esp32_p4_wifi6_touch_lcd_4_3.c').read_text()
    display = (bsp_dir / 'include/bsp/display.h').read_text()
    lcd = (ROOT / 'main/pet_p4_lcd.c').read_text()

    # Keep the production controller, config type, and both selection gates.
    controller_start = '#if BSP_LCD_PANEL_AUTO\n#include "nv3049_init.h"'
    controller = bsp[bsp.index(controller_start):].split('\n#endif', 1)[0] + '\n#endif\n'
    config = re.search(r'typedef struct \{[^{}]*\}\s*bsp_display_config_t;', display)[0]
    constructor = bsp.split('esp_err_t bsp_display_new_with_handles(', 1)[1]
    selector = '#if BSP_LCD_PANEL_AUTO\n' + constructor.split('#if BSP_LCD_PANEL_AUTO\n', 1)[1]
    selector = selector.split('\n#endif', 1)[0] + '\n#endif\n'
    panel_start = '#if BSP_LCD_PANEL_AUTO\n    if (nv3049) {\n        dpi_config.'
    panel_branch = constructor[constructor.index(panel_start):].split(
        '        ESP_LOGI(TAG, "Install Waveshare', 1,
    )[0]
    lcd_start = 'esp_err_t pet_p4_lcd_init(void) {'
    lcd_selection = lcd[lcd.index(lcd_start):].split('  if (!g_refresh_done_sem)', 1)[0]

    prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

typedef int esp_err_t;
typedef int *esp_lcd_panel_io_handle_t;
enum { ESP_OK, ESP_ERR_INVALID_STATE, PET_LCD_PANEL_ST7701, PET_LCD_PANEL_NV3049 };
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_RETURN_ON_ERROR(call, ...) do { int result = (call); if (result) return result; } while (0)
#define ESP_RETURN_ON_FALSE(ok, error, ...) do { if (!(ok)) return error; } while (0)

static int bus_io;
static int panel_storage;
static int *g_panel_handle, *g_lcd_io;
static unsigned profile_reads, touch_probes;
#if BSP_LCD_PANEL_AUTO
static bool saved_touchless, verified_touch, profile_error;
static atomic_bool g_no_touch_profile;
static atomic_int g_panel_model;

static esp_err_t load_no_touch_profile(bool *enabled) {
    ++profile_reads;
    if (profile_error) return ESP_ERR_INVALID_STATE;
    *enabled = saved_touchless;
    return ESP_OK;
}

static bool pet_p4_touch_try_nv3049(void) {
    ++touch_probes;
    return verified_touch;
}

static int xPortGetCoreID(void) { return 0; }

enum {
    BSP_LCD_RST = 17, GPIO_MODE_OUTPUT = 1,
    MIPI_DSI_LL_CLOCK_LANE_STATE_HS = 3,
    BSP_LCD_NV3049_CLOCK_MHZ = 24, LCD_COLOR_PIXEL_FORMAT_RGB888 = 24
};
#define pdMS_TO_TICKS(ms) (ms)
#define MIPI_DSI_LL_GET_HOST(bus) (bus)
typedef struct { uint64_t pin_bit_mask; int mode; } gpio_config_t;
typedef struct {
    unsigned dpi_clock_freq_mhz, pixel_format, num_fbs;
    struct { bool use_dma2d; } flags;
    struct {
        unsigned hsync_back_porch, hsync_pulse_width, hsync_front_porch;
        unsigned vsync_back_porch, vsync_pulse_width, vsync_front_porch;
    } video_timing;
} esp_lcd_dpi_panel_config_t;

static esp_err_t gpio_config(const gpio_config_t *config) {
    assert(config->pin_bit_mask == (1ULL << BSP_LCD_RST) && config->mode == GPIO_MODE_OUTPUT);
    puts("reset output");
    return ESP_OK;
}

static esp_err_t gpio_set_level(int pin, int level) {
    assert(pin == BSP_LCD_RST);
    printf("reset %d\n", level);
    return ESP_OK;
}

static void vTaskDelay(unsigned ms) { printf("wait %u\n", ms); }

static void mipi_dsi_host_ll_set_clock_lane_state(int host, int mode) {
    assert(mode == MIPI_DSI_LL_CLOCK_LANE_STATE_HS);
    printf("clock %d HS\n", host);
}

static esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int command,
                                         const void *payload, size_t bytes) {
    assert(io == &bus_io && bytes <= 1 && (payload != NULL) == (bytes != 0));
    if (bytes) {
        printf("dcs %02x 1 %02x\n", (unsigned)command, (unsigned)*(const uint8_t *)payload);
    } else {
        printf("dcs %02x 0\n", (unsigned)command);
    }
    return ESP_OK;
}

static esp_err_t esp_lcd_new_panel_dpi(int bus, const esp_lcd_dpi_panel_config_t *config,
                                      int **panel) {
    assert(bus == 0 && config->pixel_format == LCD_COLOR_PIXEL_FORMAT_RGB888);
    *panel = &panel_storage;
    puts("NV3049");
    return ESP_OK;
}

static esp_err_t esp_lcd_panel_init(int *panel) {
    assert(panel == &panel_storage);
    return ESP_OK;
}
#endif
'''
    bsp_stub_start = r'''
static esp_err_t bsp_display_new(const bsp_display_config_t *config, int **panel, int **out_io) {
#if BSP_LCD_PANEL_AUTO
    esp_err_t ret = ESP_OK;
    esp_lcd_panel_io_handle_t io = &bus_io;
    int mipi_dsi_bus = 0;
    int *panel_handle;
    esp_lcd_dpi_panel_config_t dpi_config = {0};
#define ESP_GOTO_ON_ERROR(call, label, ...) do { ret = (call); if (ret) goto label; } while (0)
#else
    assert(!config->nv3049);
#endif
'''
    bsp_stub_end = r'''
        puts("ST7701");
    }
    *panel = &panel_storage;
    *out_io = &bus_io;
    return ESP_OK;
#if BSP_LCD_PANEL_AUTO
err:
    return ret;
#endif
}
'''
    scenarios = r'''
#if BSP_LCD_PANEL_AUTO
static void run_case(const char *name, bool touchless, bool touch, bool fail_profile, int model) {
    puts(name);
    saved_touchless = touchless;
    verified_touch = touch;
    profile_error = fail_profile;
    g_panel_handle = NULL;
    profile_reads = touch_probes = 0;
    assert(pet_p4_lcd_init() == ESP_OK);
    assert(profile_reads == 1 && touch_probes == (touchless ? 0U : 1U));
    assert(atomic_load(&g_no_touch_profile) == touchless);
    assert(atomic_load(&g_panel_model) == model);
}
#endif

int main(void) {
#if BSP_LCD_PANEL_AUTO
    run_case("saved NV3049 profile", true, false, false, PET_LCD_PANEL_NV3049);
    run_case("verified touch", false, true, false, PET_LCD_PANEL_NV3049);
    run_case("unverified touch", false, false, false, PET_LCD_PANEL_ST7701);
    run_case("unreadable profile", false, true, true, PET_LCD_PANEL_NV3049);
#else
    assert(pet_p4_lcd_init() == ESP_OK);
    assert(!profile_reads && !touch_probes);
#endif
    return 0;
}
'''
    source = tmp_path / 'nv3049-init.c'
    source.write_text('\n'.join((
        prelude, config, controller, bsp_stub_start, selector, panel_branch, bsp_stub_end,
        lcd_selection, '  return ESP_OK;\n}', scenarios,
    )))

    traces = {}
    for auto in (0, 1):
        binary = tmp_path / f'nv3049-init-{auto}'
        subprocess.run(
            ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
             f'-DBSP_LCD_PANEL_AUTO={auto}', '-I', str(bsp_dir / 'priv_include'),
             str(source), '-o', str(binary)],
            check=True,
        )
        traces[auto] = subprocess.run(
            [str(binary)], check=True, capture_output=True, text=True,
        ).stdout.splitlines()

    assert traces[0] == ['ST7701']
    events = traces[1]
    assert events[0] == 'saved NV3049 profile'
    touch, legacy, failed_profile = map(
        events.index, ('verified touch', 'unverified touch', 'unreadable profile'),
    )
    verified = events[touch + 1:legacy]
    assert events[legacy + 1:failed_profile] == ['ST7701']

    # The BSP's 20 ms pre-bus wait precedes the controller reset pulses.
    reset = ['wait 20', 'reset output', 'reset 1', 'wait 10',
             'reset 0', 'wait 20', 'reset 1', 'wait 120']
    prefix = [*reset, 'clock 0 HS', 'wait 30', 'dcs 11 0', 'wait 220']
    suffix = ['dcs 29 0', 'wait 50', 'NV3049']
    assert verified[:len(prefix)] == prefix
    assert verified[-len(suffix):] == suffix

    vendor = verified[len(prefix):-len(suffix)]
    assert vendor[-4:] == ['dcs 11 1 00', 'wait 220', 'dcs 29 1 00', 'wait 50']
    # Pin the accepted vendor commands, payload bytes, order, repeats and waits.
    assert len(vendor) == 191
    assert sha256('\n'.join(vendor).encode()).hexdigest() == (
        '59c7bc41c1520a321cd47915ae8b077c03ae3c84c2f68d516e90ad8e52b3d82b'
    )
    assert events[1:touch] == verified
    assert events[failed_profile + 1:] == verified
