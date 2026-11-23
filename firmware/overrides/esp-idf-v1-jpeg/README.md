# ESP32-P4 v1 JPEG decoder backport

These files start from the pinned ESP-IDF v5.5.1 decoder and retain its DMA
interface. They incorporate the decoder changes in Espressif's v5.5 fixes
[CVE-2025-65092](https://github.com/espressif/esp-idf/commit/4b8f5859dbe05d15372558f8a950b49f6ee44e42)
and [CVE-2026-55687](https://github.com/espressif/esp-idf/commit/82c5c2dad45313786fb7e973059bc9094d95c3b2).
The COM marker change is adapted to the v5.5.1 length variable. The first
commit's encoder changes are outside this decoder backport.

CMake checks the v1 SDK files and generates the replacement decoder sources
and private header layout inside the build directory. The v3 SDK continues to
supply its own JPEG driver. Review the source checks when updating the v1 SDK.
