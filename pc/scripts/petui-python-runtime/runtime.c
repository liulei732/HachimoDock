#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "py/compile.h"
#include "py/gc.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "py/mphal.h"
#include "py/mperrno.h"

/* A fresh instance runs one job. Even on memory pressure the interpreter never
 * reclaims live objects from unscannable WebAssembly call frames. */
static _Alignas(16) unsigned char heap[4 * 1024 * 1024];
static mp_obj_t python_stack[4096];
static char source[16 * 1024];
static char input[32 * 1024];
static char result[16 * 1024];

void gc_collect(void) {
}
void nlr_jump_fail(void *value) {
    (void)value;
    __builtin_trap();
}
void mp_hal_stdout_tx_strn_cooked(const char *text, size_t len) {
    (void)text;
    (void)len;
}

static mp_obj_t deny_open(size_t count, const mp_obj_t *args, mp_map_t *kwargs) {
    (void)count;
    (void)args;
    (void)kwargs;
    mp_raise_OSError(MP_EACCES);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 0, deny_open);

#define PETUI_API __attribute__((visibility("default")))
PETUI_API char *petui_source(void) { return source; }
PETUI_API char *petui_input(void) { return input; }
PETUI_API char *petui_output(void) { return result; }

PETUI_API int petui_run(unsigned source_len, unsigned input_len) {
    if (source_len == 0 || source_len >= sizeof(source) || input_len >= sizeof(input)) {
        return -1;
    }
    mp_stack_ctrl_init();
    mp_stack_set_limit(768 * 1024);
    gc_init(heap, heap + sizeof(heap));
    mp_pystack_init(python_stack, python_stack + MP_ARRAY_SIZE(python_stack));
    mp_init();

    nlr_buf_t protected;
    if (nlr_push(&protected) != 0) {
        return -1;
    }
    mp_obj_t json = mp_import_name(MP_QSTR_json, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
    mp_obj_t loads = mp_load_attr(json, MP_QSTR_loads);
    mp_obj_t dumps = mp_load_attr(json, MP_QSTR_dumps);
    mp_obj_t data = mp_call_function_1(loads, mp_obj_new_str(input, input_len));

    mp_lexer_t *lexer = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, source, source_len, 0);
    mp_parse_tree_t tree = mp_parse(lexer, MP_PARSE_FILE_INPUT);
    mp_call_function_0(mp_compile(&tree, MP_QSTR__lt_stdin_gt_, false));
    mp_obj_t snapshot = mp_load_global(MP_QSTR_snapshot);
    mp_obj_t encoded = mp_call_function_1(dumps, mp_call_function_1(snapshot, data));
    size_t length = 0;
    const char *text = mp_obj_str_get_data(encoded, &length);
    if (length >= sizeof(result)) {
        nlr_pop();
        return -1;
    }
    memcpy(result, text, length);
    nlr_pop();
    return (int)length;
}
