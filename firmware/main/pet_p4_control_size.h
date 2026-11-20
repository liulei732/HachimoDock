#pragma once

/* WiFi6 v1 has little flash headroom. Only explicitly marked control/storage
 * metadata functions use the existing Oz profile. PCM/DSP, codec workers,
 * rendering and media consumers retain their validated performance settings.
 * Other boards and host builds keep their original function options. */
#if defined(PET_P4_WIFI6_CONTROL_SIZE) && PET_P4_WIFI6_CONTROL_SIZE \
    && defined(__GNUC__) && !defined(__clang__)
#define PET_P4_CONTROL_SIZE __attribute__((optimize("Oz")))
#else
#define PET_P4_CONTROL_SIZE
#endif
