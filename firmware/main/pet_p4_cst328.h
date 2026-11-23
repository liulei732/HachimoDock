#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#define PET_CST328_ADDRESS 0x1A

typedef struct {
    bool down, fresh;
    uint8_t count, id, strength;
    uint16_t x, y;
} pet_cst328_sample_t;

typedef struct {
    uint16_t x_resolution, y_resolution, chip_id, project_id;
} pet_cst328_info_t;

/* Startup only after 0x1A responds. Call deinit on failure.
 * The driver leaves shared-bus and GPIO reset state untouched. */
esp_err_t pet_cst328_init(i2c_master_bus_handle_t shared_bus, pet_cst328_info_t *info);
esp_err_t pet_cst328_read(pet_cst328_sample_t *sample);
void pet_cst328_deinit(void);
