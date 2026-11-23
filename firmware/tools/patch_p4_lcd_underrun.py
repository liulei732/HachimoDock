"""Generate the reviewed ESP32-P4 LCD hooks from a pinned SDK driver."""
import argparse
import hashlib
from pathlib import Path

V3_DRIVER_SHA = "6d4a59514c1401334f1a4a89933611b259ca20ef09c9670fabfc37f1dea630da"
PINNED = {
    "3aaeeefa0e7d2e12625942a67d1caf5b9400a5b99d03da0d617aabcc22321125":
        b"    if (unlikely(error_status & MIPI_DSI_LL_EVENT_UNDERRUN)) {\n",
    V3_DRIVER_SHA:
        b"    if (intr_status & MIPI_DSI_BRG_LL_EVENT_UNDERRUN) {\n",
}


def replace_once(data, before, after):
    if data.count(before) != 1:
        raise ValueError("unreviewed ESP-IDF LCD DPI driver layout")
    return data.replace(before, after, 1)


def use_optional_dma_refresh(data):
    """The NV3049 scanout retires buffers at DMA latch, not bridge VSYNC."""
    data = replace_once(
        data, b"    esp_lcd_dpi_panel_refresh_done_cb_t on_refresh_done;",
        b"    bool pet_p4_refresh_after_dma;\n    esp_lcd_dpi_panel_refresh_done_cb_t on_refresh_done;",
    )
    start = data.index(b"\n#if !MIPI_DSI_BRG_LL_EVENT_VSYNC\n")
    stop = data.index(b"\n#endif\n    return yield_needed;", start)
    # The surrounding SDK code has already started DMA from cur_fb_index.
    data = data[:start] + b"""
#if MIPI_DSI_BRG_LL_EVENT_VSYNC
    const bool refresh_after_dma = dpi_panel->pet_p4_refresh_after_dma;
#else
    const bool refresh_after_dma = true;
#endif
    if (refresh_after_dma && dpi_panel->on_refresh_done &&
        dpi_panel->on_refresh_done(&dpi_panel->base, NULL, dpi_panel->user_ctx)) {
        yield_needed = true;
    }""" + data[stop + len(b"\n#endif"):]
    data = replace_once(
        data, b"void mipi_dsi_bridge_isr_handler(void *args)",
        b"""// Called on CPU0 before registering the refresh callback.
void pet_p4_lcd_use_dma_refresh(esp_lcd_panel_handle_t panel)
{
    esp_lcd_dpi_panel_t *dpi_panel = __containerof(panel, esp_lcd_dpi_panel_t, base);
    dpi_panel->pet_p4_refresh_after_dma = true;
}

void mipi_dsi_bridge_isr_handler(void *args)""",
    )
    data = replace_once(
        data, b"    if (intr_status & MIPI_DSI_BRG_LL_EVENT_VSYNC) {",
        b"    if (!dpi_panel->pet_p4_refresh_after_dma && (intr_status & MIPI_DSI_BRG_LL_EVENT_VSYNC)) {",
    )
    return replace_once(
        data, b"MIPI_DSI_BRG_LL_EVENT_VSYNC, cbs->on_refresh_done != NULL);",
        b"MIPI_DSI_BRG_LL_EVENT_VSYNC,\n"
        b"                                    cbs->on_refresh_done != NULL && !dpi_panel->pet_p4_refresh_after_dma);",
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("original", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--dma-refresh", action="store_true")
    parser.add_argument("--skip-underrun", action="store_true")
    args = parser.parse_args()
    generated = args.original.read_bytes()
    digest = hashlib.sha256(generated).hexdigest()
    if digest not in PINNED or (args.dma_refresh and digest != V3_DRIVER_SHA):
        parser.error("unreviewed ESP-IDF LCD DPI driver")
    try:
        if not args.skip_underrun:
            declaration = b'#include "hal/color_hal.h"\n'
            generated = replace_once(generated, declaration,
                                     declaration + b"\nextern void pet_p4_lcd_on_underrun(void);\n")
            marker = PINNED[digest]
            generated = replace_once(generated, marker, marker + b"        pet_p4_lcd_on_underrun();\n")
        if args.dma_refresh:
            generated = use_optional_dma_refresh(generated)
    except (ValueError, IndexError) as err:
        parser.error(str(err))
    if not args.output.exists() or args.output.read_bytes() != generated:
        args.output.write_bytes(generated)


if __name__ == "__main__":
    main()
