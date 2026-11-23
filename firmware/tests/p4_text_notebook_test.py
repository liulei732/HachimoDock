"""Check the installable notebook against the native form and private A/B store."""
import json
from pathlib import Path
import sys

import pytest

from p4_render_trace_test import function
from petui_app_store_test import _run_private_save_generations

ROOT = Path(__file__).resolve().parents[2]
MAIN = ROOT / "firmware/main"
EXAMPLE = ROOT / "pc/skills/petui/examples/local-notebook"
sys.path.insert(0, str(ROOT / "pc/skills/petui/scripts"))
from text_notebook import notebook_files, write_package
from validate_generated_widget import DEFAULT_CAPABILITIES, validate_widget


@pytest.mark.parametrize("rows,limit", [(1,1),(2,40),(3,40),(4,63)])
def test_creator_package_and_byte_quotas(tmp_path,rows,limit):
    modes = (["latin","pinyin"] * 2)[:rows]
    files = notebook_files("local-notebook", "1.0.0", modes, limit)
    package = tmp_path / "package"
    write_package(package, files)
    caps = {**DEFAULT_CAPABILITIES, "widgetAppFormV3":"p4-app-form-v3",
            "widgetQuotas":["p4-quota-v2","p4-quota-v3"], "widgetQuota":"p4-quota-v3"}
    assert not validate_widget(package,caps)
    assert validate_widget(package,{**caps,"widgetAppFormV3":None})
    if rows==2:
        assert all(json.loads((EXAMPLE / name).read_text())==value for name,value in files.items())
    with pytest.raises(ValueError):
        notebook_files("local-notebook","2.0.0",modes,64)


def test_native_text_edits_durable_restart_and_additive_upgrade(tmp_path):
    data = [notebook_files("journal",f"{v}.0.0",["latin","pinyin"]+(["latin"] if v==2 else []))
            for v in (1,2)]
    # Embed the packages like the on-device cJSON loader. The test uses the real
    # form parser, role projection, capture/restore codec, A/B writer and epoch clear.
    driver = '\n'.join("static const char %s[]=%s;" % (name,json.dumps(json.dumps(value,ensure_ascii=False),ensure_ascii=False))
                       for name,value in (("v1",data[0]["runtime/widget.json"]),("v2",data[1]["runtime/widget.json"]),
                                          ("wide",notebook_files("journal","3.0.0",["latin","pinyin","latin","numeric"],63)["runtime/widget.json"]),
                                          ("buttons",data[0]["buttons.json"])))
    driver += r'''
#include <limits.h>
#include "pet_p4_form_v3_json.h"
#include "pet_p4_store_budget.h"
#define MINIAPP_VAR_INT 0
#define MINIAPP_MAX_VARS 32
#define MINIAPP_V2_STRING_MAX 64
#define MINIAPP_CAP(r,a,b,c) ((r)->quota_v3?32:16)
#define json_string pet_form_string
#define miniapp_runtime_lock() assert(!store_locked)
#define miniapp_runtime_unlock() assert(!store_locked)
typedef struct {char name[32],string_value[128];int type,int_value,int_min,int_max;} miniapp_var_t;
typedef struct {unsigned records[3],mode;} fruit_t;
typedef struct {
 bool active,slicing,save_page,quota_v3;struct {bool app;}surface;fruit_t fruit;pet_form3_t form3;pet_form_t form;pet_form_text_config_t form_text;
 unsigned save_version,save_vars;int state_count,current_state,current_page,page_count,var_count,save_states[4];
 char widget_id[48],states[4][24],pages[4][24];miniapp_var_t vars[32];
} miniapp_runtime_t;
static miniapp_runtime_t g_runtime;
static unsigned g_viewport_epoch;static int g_pressed_node;
static bool object_keys_allowed(const cJSON *o,const char *const *keys,int n){
 const cJSON *v;cJSON_ArrayForEach(v,o){int i=0;while(i<n&&strcmp(keys[i],v->string))i++;if(i==n)return false;}return true;
}
static void copy_utf8(char *d,size_t n,const char *s){assert(strlen(s)<n);strcpy(d,s);}
static int find_var(const miniapp_runtime_t *r,const char *n){for(int i=0;i<r->var_count;i++)if(!strcmp(n,r->vars[i].name))return i;return -1;}
static int find_name(char names[][24],int count,const char *n){for(int i=0;i<count;i++)if(!strcmp(n,names[i]))return i;return -1;}
static void refresh_view(miniapp_runtime_t*r){assert(r==&g_runtime);}
static void pet_fruit_select_mode(fruit_t*f,unsigned m){(void)f;(void)m;abort();}
'''
    source = (MAIN / "pet_p4_miniapp.c").read_text()
    workspace = source[source.index("typedef struct {\n  miniapp_var_t vars[MINIAPP_MAX_VARS];\n  cJSON nodes"):
                       source.index("static bool save_app_runtime_workspace")]
    driver += workspace + '\n'.join(function(source,name) for name in (
        "form3_owns","form3_string_limit","form3_values","form3_write","parse_save","save_json_child",
        "save_app_runtime_workspace","restore_app_runtime_buffer"))
    driver += r'''
static void open_book(const char *package,const char *id,bool expected){
 cJSON *root=cJSON_Parse(package),*b=cJSON_Parse(buttons);assert(root&&b);
 miniapp_runtime_t *r=&g_runtime;memset(r,0,sizeof(*r));
 r->quota_v3=!strcmp(pet_form_string(root,"quota"),"p4-quota-v3");
 strcpy(r->widget_id,id);r->active=r->surface.app=true;r->state_count=1;r->page_count=1;
 strcpy(r->states[0],"ready");strcpy(r->pages[0],"edit");
 const cJSON *v;cJSON_ArrayForEach(v,pet_form_get(root,"vars")){
  miniapp_var_t *var=&r->vars[r->var_count++];strcpy(var->name,v->string);var->type=!!strcmp(pet_form_string(v,"type"),"int");
 }
 assert(pet_form3_parse(root,b,&r->form3)&&parse_save(r,pet_form_get(root,"persist")));
 char json[PET_APP_SAVE_BYTES];pet_app_store_status_t status=pet_app_store_load(id,json);
 assert(status==PET_APP_STORE_EMPTY||status==PET_APP_STORE_VALUE);
 assert(restore_app_runtime_buffer(r,true,json)==expected);
 if(expected){pet_form3_value_t values[32];form3_values(r,values);assert(pet_form3_initialize(&r->form3,values));form3_write(r,values);}
 cJSON_Delete(root);cJSON_Delete(b);
}
static void action(const char *name){
 pet_form3_value_t values[32];form3_values(&g_runtime,values);
 int op=pet_form3_operation(&g_runtime.form3,name);assert(pet_form3_apply(&g_runtime.form3,values,0,op));
 form3_write(&g_runtime,values);
}
static const char *value(const char *key){return g_runtime.vars[find_var(&g_runtime,key)].string_value;}
static void sync_book(void){
 miniapp_save_workspace_t capture={0};assert(save_app_runtime_workspace(&capture));
 save_io_t remove={0};assert(save_io_begin(&remove,NULL));
 assert(pet_app_store_preserve_with_lease(g_runtime.widget_id,remove.lease));save_io_end(&remove);
 memset(saves,0,sizeof(saves)); /* next open must read physical generations */
}
int main(void){
 assert(pet_app_store_init());open_book(v1,"journal",true);
 action("form.accept");action("text.append");action("field.next");action("text.append");action("form.accept");
 assert(!strcmp(value("note1"),"ab"));action("field.next");action("form.accept");
 action("text.append");action("form.accept");action("form.accept");
 char chinese[64];strcpy(chinese,value("note2"));assert(strlen(chinese)==3&&pet_form_utf8(chinese,3,NULL,NULL,NULL));
 action("form.accept");action("text.erase");action("form.cancel");assert(!strcmp(value("note2"),chinese));
 // A draft can change without affecting either published row or the saved file.
 action("form.accept");action("text.erase");assert(!value("note2Draft")[0]);sync_book();
 open_book(v1,"journal",true);assert(!strcmp(value("note1"),"ab")&&!strcmp(value("note2"),chinese));
 open_book(v1,"other",true);assert(!value("note1")[0]&&!value("note2")[0]);
 open_book(v2,"journal",true);assert(!strcmp(value("note1"),"ab")&&!strcmp(value("note2"),chinese)&&!value("note3")[0]);
 action("field.next");action("field.next");action("form.accept");
 pet_form3_value_t values[32];form3_values(&g_runtime,values);
 const char *boundary="1234567890123456789012345678901234567中";assert(strlen(boundary)==40);
 assert(pet_form3_replace(&g_runtime.form3,values,0,2,boundary)==PET_FORM_REPLACE_OK);
 assert(pet_form3_replace(&g_runtime.form3,values,0,2,"12345678901234567890123456789012345678中")==PET_FORM_REPLACE_LIMIT);
 assert(pet_form3_replace(&g_runtime.form3,values,0,2,"\xC0\x80")==PET_FORM_REPLACE_INVALID);
 form3_write(&g_runtime,values);action("form.accept");sync_book();
 open_book(v1,"journal",false); /* dropping a durable row must refuse */
 open_book(v2,"journal",true);assert(!strcmp(value("note3"),boundary)&&!strcmp(value("note2"),chinese));
 action("field.next");action("form.accept");action("text.erase");action("form.accept");sync_book();
 open_book(v2,"journal",true);assert(!value("note2")[0]&&!strcmp(value("note1"),"ab")&&!strcmp(value("note3"),boundary));
 open_book(wide,"journal",true);assert(g_runtime.quota_v3&&!strcmp(value("note3"),boundary));
 action("field.next");action("field.next");action("field.next");action("form.accept");action("text.append");action("form.accept");
 char number[64];strcpy(number,value("note4"));assert(strlen(number)==1);sync_book();
 open_book(wide,"journal",true);assert(!strcmp(value("note4"),number)&&!strcmp(value("note1"),"ab")&&!strcmp(value("note3"),boundary));
 return 0;
}
'''
    # GCC diagnoses the compact layout of the extracted shipping form code.
    _run_private_save_generations(
        tmp_path, False, record_driver=driver,
        record_cflags=["-DPET_FORM_V2_ENABLED=1", "-DPET_FORM_V3_ENABLED=1",
                       "-Wno-error=misleading-indentation"],
        extra_sources=[MAIN/"pet_p4_form_v2.c", MAIN/"pet_p4_form_v3.c",
                       ROOT/"firmware/tests/vendor/cjson/cJSON.c"],
    )
