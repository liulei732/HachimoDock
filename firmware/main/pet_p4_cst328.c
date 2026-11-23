// CST328 register protocol: Hynitron CST328 datasheet V2.2, sections 10 and 12.
// Reference URLs are retained in the touch diagnostic record. No firmware/config
// programming is performed: only information/normal mode and report ACK.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "pet_p4_cst328.h"

static const char *TAG = "pet-cst328";
static i2c_master_dev_handle_t touch;

static esp_err_t read_reg(uint16_t reg, uint8_t *buf, size_t size)
{
    uint8_t address[] = {reg >> 8, reg & 0xFF};
    return i2c_master_transmit_receive(touch, address, sizeof(address), buf, size, 50);
}
static esp_err_t command(uint16_t reg)
{
    uint8_t bytes[] = {reg >> 8, reg & 0xFF};
    return i2c_master_transmit(touch, bytes, sizeof(bytes), 50);
}
static esp_err_t write_byte(uint16_t reg, uint8_t value)
{
    uint8_t bytes[] = {reg >> 8, reg & 0xFF, value};
    return i2c_master_transmit(touch, bytes, sizeof(bytes), 50);
}
static uint16_t le16(const uint8_t *bytes)
{
    return bytes[0] | ((uint16_t)bytes[1] << 8);
}

esp_err_t pet_cst328_init(i2c_master_bus_handle_t bus, pet_cst328_info_t *info)
{
    if (!bus || !info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    if (touch) return ESP_ERR_INVALID_STATE;
    esp_err_t err;
    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PET_CST328_ADDRESS,
        .scl_speed_hz = 400000,
    };
    if ((err = i2c_master_bus_add_device(bus, &dev, &touch)) != ESP_OK) return err;
    if ((err = command(0xD101)) != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t boot[4] = {0}, id[4] = {0}, resolution[4] = {0}, version[4] = {0};
    err = read_reg(0xD1FC, boot, 4);
    if (err == ESP_OK) err = read_reg(0xD204, id, 4);
    if (err == ESP_OK) err = read_reg(0xD1F8, resolution, 4);
    if (err == ESP_OK) err = read_reg(0xD208, version, 4);
    // Restore normal reporting even if one diagnostic read failed.
    esp_err_t normal = command(0xD109);
    vTaskDelay(pdMS_TO_TICKS(20));
    if (err != ESP_OK) return err;
    if (normal != ESP_OK) return normal;
    ESP_LOGI(TAG, "Info bytes boot=%02X%02X%02X%02X firmware=%02X%02X%02X%02X",
             boot[0], boot[1], boot[2], boot[3], version[0], version[1], version[2], version[3]);
    if (le16(boot + 2) != 0xCACA) {
        ESP_LOGE(TAG, "Unexpected info marker 0x%04X; not assuming CST328 protocol",le16(boot + 2));
        return ESP_ERR_INVALID_RESPONSE;
    }
    info->project_id = le16(id);
    info->chip_id = le16(id + 2);
    info->x_resolution = le16(resolution);
    info->y_resolution = le16(resolution + 2);
    if (info->x_resolution < 100 || info->x_resolution > 4096 ||
        info->y_resolution < 100 || info->y_resolution > 4096) return ESP_ERR_INVALID_SIZE;
    if (info->chip_id != 0x0148 || info->project_id != 0x4B05 ||
        info->x_resolution != 240 || info->y_resolution != 320) return ESP_ERR_NOT_SUPPORTED;
    return write_byte(0xD000, 0xAB);
}

esp_err_t pet_cst328_read(pet_cst328_sample_t *sample)
{
    if (!sample || !touch) return ESP_ERR_INVALID_ARG;
    memset(sample, 0, sizeof(*sample));
    // The UI dispatches one finger; read the complete first-point header only.
    uint8_t data[7] = {0};
    esp_err_t err = read_reg(0xD000, data, sizeof(data));
    if (err != ESP_OK) return err;
    uint8_t count = data[5] & 0x0F;
    // Our previous ACK leaves D000=AB. Do not clear a controller report again
    // while waiting for a fresh one; that could discard a newly arriving sample.
    if (data[0] == 0xAB) return ESP_OK;
    esp_err_t clear = write_byte(0xD005, 0);
    esp_err_t ack = write_byte(0xD000, 0xAB);
    if (clear != ESP_OK) return clear;
    if (ack != ESP_OK) return ack;
    if (count > 5 || (count > 0 && data[6] != 0xAB)) return ESP_ERR_INVALID_RESPONSE;
    sample->fresh = true;
    sample->count = count;
    sample->down = count > 0 && (data[0] & 0x0F) == 6;
    sample->id = data[0] >> 4;
    sample->x = ((uint16_t)data[1] << 4) | (data[3] >> 4);
    sample->y = ((uint16_t)data[2] << 4) | (data[3] & 0x0F);
    sample->strength = data[4];
    return ESP_OK;
}

void pet_cst328_deinit(void)
{
    if (touch && i2c_master_bus_rm_device(touch) == ESP_OK) touch = NULL;
    // The BSP owns the shared bus and codec; never destroy it here.
}
