# SPDX-License-Identifier: Apache-2.0
# Local BSP adaptation: dependency/build settings differ from the published component.
# See this component LICENSE and licenses/DEPENDENCY-LICENSE-NOTICE.md.

idf_component_register(
    SRCS "esp32_p4_wifi6_touch_lcd_4_3.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES driver
    PRIV_REQUIRES esp_lcd usb spiffs fatfs
)

if("$ENV{PET_P4_LCD_PANEL_AUTO}" STREQUAL "1")
    idf_build_get_property(panel_idf_version IDF_VER)
    if("$ENV{PET_P4_CHIP_FAMILY}" STREQUAL "v1")
        set(panel_validated_sdk "^v?5[.]5[.]1([.-]|$)")
    elseif("$ENV{PET_P4_CHIP_FAMILY}" STREQUAL "v3")
        set(panel_validated_sdk "^v?5[.]5[.]4([.-]|$)")
        target_compile_definitions(${COMPONENT_LIB} PUBLIC BSP_LCD_NV3049_DMA_REFRESH=1)
    else()
        message(FATAL_ERROR "Unreviewed ESP32-P4 LCD silicon family")
    endif()
    if(NOT panel_idf_version MATCHES "${panel_validated_sdk}" OR NOT CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0)
        message(FATAL_ERROR "NV3049 requires the pinned ESP-IDF and CPU0 scanout fence")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PUBLIC BSP_LCD_PANEL_AUTO=1)
endif()
