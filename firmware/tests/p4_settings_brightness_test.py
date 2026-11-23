"""Compile the actual LCD preference routines with a simulated panel and NVS."""
from pathlib import Path
import subprocess


def test_brightness_apply_persist_rollback_and_fixed_panel(tmp_path):
    source = (Path(__file__).parents[1] / "main/pet_p4_lcd.c").read_text()
    routines = source[source.index("int pet_p4_lcd_brightness("):source.index("#endif", source.index("bool pet_p4_lcd_save_brightness("))]
    harness = tmp_path / "brightness.c"
    harness.write_text(r"""
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <assert.h>
typedef int esp_err_t, nvs_handle_t;
enum { ESP_OK, NVS_READONLY, NVS_READWRITE };
static atomic_int panel_brightness=-1;
static int current, saved, pending, fail_open, fail_commit, fail_panel, opens;
static int nvs_open(const char *ns,int mode,int *out) {
  assert(!strcmp(ns,"pet_lcd")); (void)mode; ++opens; *out=1; return fail_open;
}
static int nvs_get_u8(int h,const char *key,uint8_t *v) {
  assert(h==1 && !strcmp(key,"brightness")); *v=saved; return 0;
}
static int nvs_set_u8(int h,const char *key,uint8_t v) {
  assert(h==1 && !strcmp(key,"brightness")); pending=v; return 0;
}
static int nvs_commit(int h) {assert(h==1); if(fail_commit)return 1; saved=pending; return 0;}
static void nvs_close(int h) {assert(h==1);}
static int bsp_display_brightness_set(int v) {if(fail_panel==v)return 1; current=v; return 0;}
""" + routines + r"""
int main(void) {
  assert(!pet_p4_lcd_save_brightness(40) && !opens);
  fail_open=1; assert(load_brightness()==50); fail_open=0;
  saved=255; assert(load_brightness()==50);
  saved=60; assert(load_brightness()==60); current=panel_brightness=60;
  assert(!pet_p4_lcd_save_brightness(9) && !pet_p4_lcd_save_brightness(101));
  assert(pet_p4_lcd_save_brightness(30) && saved==30 && current==30 && panel_brightness==30);
  int before=opens; assert(pet_p4_lcd_save_brightness(30) && opens==before);
  fail_panel=40; assert(!pet_p4_lcd_save_brightness(40) && current==30 && saved==30);
  fail_commit=1; assert(!pet_p4_lcd_save_brightness(50) && current==30 && panel_brightness==30);
  fail_panel=30; assert(!pet_p4_lcd_save_brightness(50) && panel_brightness==-1);
}
""")
    binary = tmp_path / "brightness"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", str(harness), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
