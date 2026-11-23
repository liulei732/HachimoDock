# Build from an unpacked MicroPython v1.29.0 and WASI SDK 34.
# Invoke from a scratch directory with PORT set to this directory.
TARGET_FLAGS = -mllvm -wasm-enable-sjlj -fwasm-exceptions -DNDEBUG
CFLAGS += -I$(PORT) -I$(MICROPYTHON_TOP)/ports/embed $(TARGET_FLAGS)
SRC_QSTR += $(MICROPYTHON_TOP)/extmod/modjson.c $(PORT)/runtime.c
include $(MICROPYTHON_TOP)/ports/embed/embed.mk

.PHONY: wasm
wasm: micropython-embed-package
	$(CC) $(TARGET_FLAGS) -std=c11 -Os -fvisibility=hidden -I$(PORT) -I$(PACKAGE_DIR) \
	  $(PORT)/runtime.c $(PACKAGE_DIR)/py/*.c $(MICROPYTHON_TOP)/extmod/modjson.c \
	  -mexec-model=reactor -lsetjmp \
	  -Wl,--export=petui_source,--export=petui_input,--export=petui_output,--export=petui_run \
	  -Wl,--strip-all,-z,stack-size=1048576,--initial-memory=6291456,--max-memory=6291456 \
	  -o python.wasm
