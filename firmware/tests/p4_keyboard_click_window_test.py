"""Exercise the shipping click arbitration routines with a deterministic device clock."""
from pathlib import Path
import subprocess


def test_device_and_bluetooth_click_windows(tmp_path):
    src = (Path(__file__).parents[1] / "main/pet_p4_input.c").read_text()
    device = src[src.index("#define DEVICE_DOUBLE_PRESS_MS"):src.index("static void process_button_event(")]
    keyboard = src[src.index("#define BLE_DOUBLE_MS"):src.index("static bool keyboard_input(")]
    harness = tmp_path / "click.c"
    harness.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define PET_P4_PAGE_MAX 24
#define PET_P4_INPUT_EVENT_MAX 48
#define PET_P4_INPUT_CONTROL_SW3 2
#define PET_VOICE_DRAFT 1
typedef struct {unsigned control; unsigned long long ts_ms;} pet_p4_input_event_t;
typedef struct {bool approval_active,session_voice_active,conversation;char screen_page[24];
                struct {int phase;} voice_feedback;} pet_p4_runtime_state_t;
typedef struct {const char *action;} pet_p4_input_binding_t;
typedef void (*pet_p4_send_line_fn)(void);
static unsigned g_input_config_epoch,component,shorts,doubles;
static bool app_double,global_back;
static pet_p4_input_binding_t binding={"disabled"};
static const char *button_event_prefix(unsigned control){assert(control==2);return "button.sw3";}
static bool pet_p4_conversation_active(const pet_p4_runtime_state_t *s){return s->conversation;}
static unsigned pet_p4_miniapp_input_context(void){return component;}
static bool pet_p4_miniapp_has_input(const char *e){return app_double && strstr(e,"double_press");}
static bool active_global_priority_binding(const pet_p4_runtime_state_t *s,const char *e){
  return global_back && !strcmp(s->screen_page,"app") && strstr(e,"short_press");
}
static const pet_p4_input_binding_t *active_binding(const char *e){assert(strstr(e,"double_press"));return &binding;}
static void copy_text(char *d,size_t n,const char *s){snprintf(d,n,"%s",s);}
static void dispatch_binding_event(pet_p4_runtime_state_t *s,pet_p4_send_line_fn send,void *ctx,
  const pet_p4_input_event_t *e,const char *name,const char *gesture){
  (void)s;(void)send;(void)ctx;assert(e->control==2 && strstr(name,gesture));
  if(!strcmp(gesture,"short_press")) shorts++; else {assert(!strcmp(gesture,"double_press"));doubles++;}
}
static unsigned chords[20],sent;
static unsigned pet_ble_keyboard_chord(unsigned i){return chords[i];}
static void pet_ble_keyboard_send(unsigned chord){assert(!sent);sent=chord;}
''' + device + keyboard + r'''
int main(void){
  pet_p4_runtime_state_t s={.screen_page="main"};
  pet_p4_input_event_t e={.control=2,.ts_ms=1000};
  const char *short_event="button.sw3.short_press";
  assert(!defer_device_click(&s,NULL,NULL,&e,short_event));
  binding.action="session_next";
  assert(defer_device_click(&s,NULL,NULL,&e,short_event));
  finish_pending_clicks(&s,NULL,NULL,1279);assert(!shorts);
  finish_pending_clicks(&s,NULL,NULL,1280);assert(shorts==1);
  e.ts_ms=2000;assert(defer_device_click(&s,NULL,NULL,&e,short_event));
  e.ts_ms=2200;assert(defer_device_click(&s,NULL,NULL,&e,short_event));
  finish_pending_clicks(&s,NULL,NULL,2500);assert(shorts==1 && doubles==1);
  strcpy(s.screen_page,"components");assert(!defer_device_click(&s,NULL,NULL,&e,short_event));
  strcpy(s.screen_page,"app");app_double=true;global_back=true;
  assert(!defer_device_click(&s,NULL,NULL,&e,short_event));
  global_back=false;assert(defer_device_click(&s,NULL,NULL,&e,short_event));
  component++;finish_pending_clicks(&s,NULL,NULL,2999);assert(shorts==1);
  s.approval_active=true;assert(!defer_device_click(&s,NULL,NULL,&e,short_event));
  chords[6]=11;keyboard_click(2,1000);assert(sent==11);sent=0;
  chords[7]=12;keyboard_click(2,2000);keyboard_tick(2279);assert(!sent);
  keyboard_click(2,2279);assert(sent==12);sent=0;keyboard_tick(2400);assert(!sent);
  keyboard_click(2,3000);keyboard_tick(3282);assert(sent==11);
  return 0;
}
''')
    binary = tmp_path / "click"
    subprocess.run(["cc", "-std=c11", "-fsanitize=address,undefined", str(harness), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
