#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cst328_host_stubs.h"
#include "pet_p4_cst328.h"

static int bus_object, dev_object;
static unsigned adds, removes, normal, clears, acks;
static bool bad_marker, info_failure, bad_resolution, wrong_project, report_failure;
static uint8_t report[7];

void vTaskDelay(unsigned ms) { (void)ms; }
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                  i2c_master_dev_handle_t *dev) {
  assert(bus == &bus_object && cfg->device_address == 0x1A && cfg->scl_speed_hz == 400000);
  ++adds; *dev = &dev_object; return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev) {
  assert(dev == &dev_object); ++removes; return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *data, size_t size, int timeout) {
  assert(dev == &dev_object && size >= 2); (void)timeout;
  unsigned reg = ((unsigned)data[0] << 8) | data[1];
  if (size == 2) { assert(reg == 0xD101 || reg == 0xD109); if (reg == 0xD109) ++normal; }
  else {
    assert(size == 3);
    if (reg == 0xD005) { assert(data[2] == 0); ++clears; }
    else { assert(reg == 0xD000 && data[2] == 0xAB); ++acks; }
  }
  return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *address,
                                    size_t address_size, uint8_t *out, size_t size, int timeout) {
  assert(dev == &dev_object && address_size == 2); (void)timeout;
  unsigned reg = ((unsigned)address[0] << 8) | address[1];
  if (reg == 0xD000) {
    assert(size == 7); if (report_failure) return ESP_ERR_TIMEOUT;
    memcpy(out, report, 7); return ESP_OK;
  }
  assert(size == 4); memset(out, 0, size);
  switch (reg) {
    case 0xD1FC: out[2] = bad_marker ? 0 : 0xCA; out[3] = 0xCA; break;
    case 0xD204:
      if (info_failure) return ESP_ERR_TIMEOUT;
      out[0] = wrong_project ? 0x06 : 0x05; out[1] = 0x4B; out[2] = 0x48; out[3] = 0x01; break;
    case 0xD1F8: out[0] = bad_resolution ? 0 : 240; out[2] = 0x40; out[3] = 1; break;
    case 0xD208: break;
    default: assert(false);
  }
  return ESP_OK;
}
static void reset_fixture(void) {
  pet_cst328_deinit();
  adds = removes = normal = clears = acks = 0;
  bad_marker = info_failure = bad_resolution = wrong_project = report_failure = false;
  memset(report, 0, sizeof(report));
}
int main(void) {
  pet_cst328_info_t info;
  pet_cst328_sample_t sample;
  reset_fixture();
  assert(pet_cst328_init(&bus_object, &info) == ESP_OK);
  assert(adds == 1 && normal == 1 && acks == 1);
  assert(info.chip_id == 0x0148 && info.project_id == 0x4B05);
  assert(info.x_resolution == 240 && info.y_resolution == 320);
  reset_fixture(); info_failure = true;
  assert(pet_cst328_init(&bus_object, &info) == ESP_ERR_TIMEOUT && normal == 1);
  reset_fixture(); bad_marker = true;
  assert(pet_cst328_init(&bus_object, &info) == ESP_ERR_INVALID_RESPONSE);
  reset_fixture(); bad_resolution = true;
  assert(pet_cst328_init(&bus_object, &info) == ESP_ERR_INVALID_SIZE);
  reset_fixture(); wrong_project = true;
  assert(pet_cst328_init(&bus_object, &info) == ESP_ERR_NOT_SUPPORTED && acks == 0);
  reset_fixture();
  assert(pet_cst328_init(&bus_object, &info) == ESP_OK);
  assert(pet_cst328_init(&bus_object, &info) == ESP_ERR_INVALID_STATE);
  report[0] = 0xAB;
  assert(pet_cst328_read(&sample) == ESP_OK && !sample.fresh && acks == 1 && clears == 0);
  // Contact 2 at (239,319): 12-bit coordinates packed across bytes 1,2,3.
  const uint8_t down[7] = {0x26, 0x0E, 0x13, 0xFF, 80, 1, 0xAB};
  memcpy(report, down, 7);
  assert(pet_cst328_read(&sample) == ESP_OK && sample.fresh && sample.down);
  assert(sample.id == 2 && sample.x == 239 && sample.y == 319 && sample.strength == 80);
  assert(acks == 2 && clears == 1);
  report[5] = 0;
  assert(pet_cst328_read(&sample) == ESP_OK && sample.fresh && !sample.down);
  report[5] = 6;
  assert(pet_cst328_read(&sample) == ESP_ERR_INVALID_RESPONSE);
  report[5] = 1; report[6] = 0;
  assert(pet_cst328_read(&sample) == ESP_ERR_INVALID_RESPONSE);
  report_failure = true;
  unsigned old_acks = acks;
  assert(pet_cst328_read(&sample) == ESP_ERR_TIMEOUT && acks == old_acks);
  pet_cst328_deinit(); pet_cst328_deinit(); assert(removes == 1);
  assert(pet_cst328_read(&sample) == ESP_ERR_INVALID_ARG);
  puts("CST328 non-resetting identity, ACK and report failure fixtures passed");
  return 0;
}
