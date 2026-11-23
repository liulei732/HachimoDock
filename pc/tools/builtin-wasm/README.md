# 内置 WASM 组件维护

模拟琴键、2048 和象棋的 C 逻辑和绘图源来自原 virtual-device 工程。此目录是三个内置包的可重建输入。消星星沿用社区作者提供的预编译模块，随包文案、封面和动作回归可单独维护。

使用仓库 PetUI SDK、可编译 wasm32 的 Clang 和 wasm-ld。琴键和象棋原画使用 Pillow 13.0.0。将 LVGL 的 NotoSansSC-Regular.ttf、Montserrat-Bold.ttf、Montserrat-Medium.ttf 放在同一个字体目录，通过 HACHI_ART_FONTS 指定。更新单个模块时运行以下命令。

```sh
export PYTHONDONTWRITEBYTECODE=1
CLANG=/path/to/clang WASM_LD=/path/to/wasm-ld HACHI_ART_FONTS=/path/to/fonts \
  python3 pc/tools/builtin-wasm/rebuild.py wasm-piano
node --test pc/tools/builtin-wasm/actions.test.mjs
petui-wasm-check pc/builtin-clawpkgs/wasm-piano/runtime/app.wasm pc/tools/builtin-wasm/wasm-piano/game.trace
```

重建命令同步该包的 app.wasm 与 widget.json 内的摘要。每个 game.trace 使用包自身的输入和画布坐标；消星星的样例回放种子为 8。实体按键映射、存档回读、音符频率和终局断言见 actions.test.mjs。
