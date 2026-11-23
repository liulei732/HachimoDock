# 设备离线精选与玩法实验

在组件中心打开「每天一本书」「每日新知」、消星星或番茄钟的详情，使用对应的“导入”按钮，再从本机组件库预览并保存到设备。书袋、知识精选和每日消星各有独立组件 ID，可与原组件并存。番茄钟沿用组件 ID，导入为另一个本地版本，可以在详情切换来源。预览与设备分开存档。

## 阅读资源与兼容性

两款精选包共用 reader.c 和同一内容接口。包内 runtime/widget.json 的 offlineReading 值为 embedded-shelf-v1，hachiWasm 是 runtime/app.wasm 的 SHA-256，hachiCanvas 是 640x480-v1。前者供功能标签识别。正文、文章 ID 索引和逐页文本都已编进经校验的 WASM；设备按当前组件直接读取。两个包均不声明 PC 服务或 autoRead。

输入取自原组件 pc/application.json 的 content 数组，每篇包含 id、title 和 text，来源、链接、修订和许可随包写入 licenses/selection.txt。缺省精选分别为 3 篇导读和 5 条知识。构建时可重复传 --article，指定 1 至 32 个不同的原始 ID。生成器检查全库书签键唯一性，拒绝缺失 ID、空正文或大于 64 KiB 的模块。新包在临时目录完成校验与 WAMR 回放后才替换输出文件。

目录支持触屏和摇杆。SW1 打开或前进，SW2 返回目录。每次打开或翻页更新书签：设备按组件 ID 保存槽位，槽 0 为文章 ID 的 CRC32 低 31 位，槽 1 为从零起算的页码。重排目录保留书签，文章移出精选则返回目录，正文缩短则停在最后一页。固件按约两秒间隔以及切换组件时提交变化，突然断电可能回退到上次写入的位置。

设备需声明 widgetWasm=hachi-wasm-v1 和 widgetWasmCanvas=640x480-v1。缺少相应能力的旧板可继续使用原版桌面联动阅读。默认精选适配 64 KiB 的模块额度和两页固定 WASM 内存。

## 玩法与存档

- 每日消星包另需 widgetWasmHost=hachi-host-v1。自由局使用设备随机数；每日局按就绪的当地日期产生种子，可查看昨日最佳。当设备时间不可用时仍可开自由局。主键消除相邻同色星，副键返回选局。替换进行中的棋局需要再次确认，结算可重玩原种子。
- 棋盘 8×7。槽 0 至 7 依次保存初始种子、两段消除掩码、分数、开局日期、最近两项每日结算记录和版本校验值。按固定算法重建颜色与重力后的布局。改变生成或重建算法需同时为既有存档保留解码路径。
- 番茄钟 1.3.5 保留原计时变量、状态及持久化字段。反馈每次打开默认关闭，可触控或用摇杆左右切换。自然完成一轮后，在休息页显示四秒；跳过不触发。组件中心使用导入时的当前桌宠，换宠后可重新导入。只有视频或图片不可用时显示宠物名字。直接导入目录中的通用包使用默认形象。反馈只改变前景画面。

## 本地构建与验证

从源码根目录运行，生成包位于指定输出目录。依赖 Python 3、Pillow、Node、PetUI C 工具链和配套的原生 PetUI CLI。--font 是生成桌面封面所用的本机中文字库。也可用 --desktop 指定带有 authoring 资源的桌面程序，代替 --petui。

```sh
python3 pc/tools/offline-components/build.py book-pocket /tmp/petui-shelves \
  --article book-061 --article thinking-in-systems --version 1.0.1 \
  --clang /path/to/clang --linker /path/to/wasm-ld \
  --font /path/to/cjk-font --petui /path/to/petui
PETUI_UPGRADE_SHELF=/tmp/petui-shelves/book-pocket-offline.hachiapp \
  node --test pc/tools/offline-components/editions.test.mjs
cargo test --manifest-path pc/src-tauri/Cargo.toml --lib \
  import_offline_editions_through_real_library_and_personalize_focus
```

替换包测试使用上例的选文顺序，同时验证保留和移除书签。其余包可用 hachimi-learn、cozy-pop-stars 或 tomato-clock 代替构建目标；番茄钟可用 --portrait 指定本地图片。测试驱动实际打包的组件，Rust 用临时组件库检查导入、个性化与设备传输准备。实体板断电、触控手感、不同屏幕上的字形以及定时器长稳仍需真机验收。
