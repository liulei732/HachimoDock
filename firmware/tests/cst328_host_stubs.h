/* Bus/GPIO failure injection only; these types do not validate target SDK ABI. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_TIMEOUT,
       ESP_ERR_INVALID_RESPONSE, ESP_ERR_INVALID_SIZE, ESP_ERR_NOT_FOUND, ESP_ERR_NOT_SUPPORTED };
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct { int dev_addr_length, device_address, scl_speed_hz; } i2c_device_config_t;
enum { I2C_ADDR_BIT_LEN_7 };
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
void vTaskDelay(unsigned ms);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                  i2c_master_dev_handle_t *dev);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *address,
                                    size_t address_size, uint8_t *out, size_t size, int timeout);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *data, size_t size, int timeout);
