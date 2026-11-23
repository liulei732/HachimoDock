# PetUI Python runtime build

The desktop packages share the WebAssembly binary in `pc/skills/petui/runtime/python`. It uses the MicroPython embed port and has an empty WebAssembly import table. The bundled Node launcher verifies that property and the runtime digest before instantiation.

- MicroPython [v1.29.0](https://github.com/micropython/micropython/releases/tag/v1.29.0), commit `0fd6c573ea815774668bbb16b8e197c8822368b2`. Archive SHA-256 `8072f6ad42c0e3bbfca88ed09bf8f1106ce207a39129af920f70081b8f2fe286`.
- [WASI SDK 34](https://github.com/WebAssembly/wasi-sdk/releases/tag/wasi-sdk-34). The macOS arm64 SDK archive SHA-256 is `9c59398106b417f8f14913380fdf0097a8cc0ff4af9eb3ce0065a859e88d49e9`.

In an empty build directory, set `PORT`, `MICROPYTHON_TOP` and `WASI_SDK` to the absolute paths of this directory, the unpacked MicroPython source and the SDK. Run:

```sh
ln -s "$PORT/mpconfigport.h" mpconfigport.h
PATH="$WASI_SDK/bin:$PATH" make -f "$PORT/Makefile" wasm \
  PORT="$PORT" MICROPYTHON_TOP="$MICROPYTHON_TOP" CC="$WASI_SDK/bin/clang"
shasum -a 256 python.wasm
```

On interpreter upgrades, update the launcher digest and license files, then run the tests in `pc/src/component-center/python-runtime.test.js`. The package’s single entry point is called once per instance. The interpreter has fixed memory, receives data through linear memory and returns only a JSON result. The host process owns the deadline.
