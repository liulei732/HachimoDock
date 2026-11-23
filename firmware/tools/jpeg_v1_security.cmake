# The v1 toolchain is pinned to IDF 5.5.1, including its JPEG DMA ABI.
if(NOT IDF_TARGET STREQUAL "esp32p4" OR NOT CONFIG_SOC_JPEG_DECODE_SUPPORTED
    OR NOT CONFIG_ESP_REV_MAX_FULL LESS 300)
  return()
endif()

function(pet_p4_checked_jpeg_source input expected_sha)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
  file(SHA256 "${input}" actual_sha)
  if(NOT actual_sha STREQUAL expected_sha)
    message(FATAL_ERROR "Review the v1 JPEG backport against the unrecognized SDK source: ${input}")
  endif()
endfunction()

idf_component_get_property(jpeg_lib esp_driver_jpeg COMPONENT_LIB)
idf_component_get_property(jpeg_dir esp_driver_jpeg COMPONENT_DIR)
get_filename_component(jpeg_dir "${jpeg_dir}" REALPATH)
set(jpeg_backport "${CMAKE_CURRENT_LIST_DIR}/../overrides/esp-idf-v1-jpeg")
set(jpeg_output "${CMAKE_BINARY_DIR}/pet_p4_jpeg_v1")

pet_p4_checked_jpeg_source("${jpeg_dir}/jpeg_decode.c"
  b852e976e710a54adc338b2721f5df9dc70559ab0b12a881bec7227d21174fe7)
pet_p4_checked_jpeg_source("${jpeg_dir}/jpeg_parse_marker.c"
  614422f6cdf36ffa52940addc9288ca02ffa3f118eb238c14f8941eedd4c0bc2)
pet_p4_checked_jpeg_source("${jpeg_dir}/jpeg_private.h"
  272cf101611965f555f942be6f4fbebfd7d1fbca78741e6b3b93f8b62cd50ba3)
pet_p4_checked_jpeg_source("${jpeg_dir}/private/jpeg_param.h"
  4da3bc5e92f1c43bfe59613780b6b262d97faa470c8cf23931fab6b7a0701ee5)
pet_p4_checked_jpeg_source("${jpeg_dir}/private/jpeg_parse_marker.h"
  32c54246ffa7c62accab09b28bb574619d4e62249dfb10640f0923ae343baee8)

# Both SDK headers include ../jpeg_private.h. Preserve that layout so both
# decoder translation units see the same patched private structure.
foreach(jpeg_header IN ITEMS private/jpeg_param.h private/jpeg_parse_marker.h)
  configure_file("${jpeg_dir}/${jpeg_header}" "${jpeg_output}/${jpeg_header}" COPYONLY)
endforeach()
foreach(jpeg_file IN ITEMS jpeg_decode.c jpeg_parse_marker.c jpeg_private.h)
  configure_file("${jpeg_backport}/${jpeg_file}" "${jpeg_output}/${jpeg_file}" COPYONLY)
endforeach()

get_target_property(jpeg_sources ${jpeg_lib} SOURCES)
foreach(jpeg_file IN ITEMS jpeg_decode.c jpeg_parse_marker.c)
  set(jpeg_matches 0)
  set(jpeg_fixed_sources "")
  foreach(jpeg_source IN LISTS jpeg_sources)
    get_filename_component(jpeg_absolute "${jpeg_source}" REALPATH BASE_DIR "${jpeg_dir}")
    if(jpeg_absolute STREQUAL "${jpeg_dir}/${jpeg_file}")
      list(APPEND jpeg_fixed_sources "${jpeg_output}/${jpeg_file}")
      math(EXPR jpeg_matches "${jpeg_matches}+1")
    else()
      list(APPEND jpeg_fixed_sources "${jpeg_source}")
    endif()
  endforeach()
  if(NOT jpeg_matches EQUAL 1)
    message(FATAL_ERROR "The v1 JPEG security backport must replace exactly one ${jpeg_file}")
  endif()
  set(jpeg_sources "${jpeg_fixed_sources}")
endforeach()
set_property(TARGET ${jpeg_lib} PROPERTY SOURCES "${jpeg_sources}")
# ESP-IDF assertions can put __FILE__ in the app image. Keep the SDK-style
# relative name for the generated files too.
target_compile_options(${jpeg_lib} PRIVATE "-fmacro-prefix-map=${jpeg_output}=esp_driver_jpeg")
message(STATUS "ESP32-P4 v1 JPEG security backport enabled")
