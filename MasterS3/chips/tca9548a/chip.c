// Wokwi custom chip — TCA9548A 8-channel I2C mux.
//
// Phase 2 scope (#63 / #58): minimal stub. Acknowledges I2C transactions
// at address 0x70 and stores the channel-select byte. Does NOT route the
// downstream channels (SD0..SD7 / SC0..SC7) yet — that's Phase 5 (#58),
// when MasterS3 starts bank-switching real units behind the mux.
//
// Real TCA9548A behavior (for the Phase 5 expansion):
//   - I2C address 0x70 + (A2,A1,A0)
//   - Single 8-bit control register: each bit enables a downstream channel
//   - Multiple bits set → multiple channels enabled in parallel
//   - When channel(s) enabled, all I2C traffic on the upstream bus is
//     mirrored to those channels and responses bridged back
//   - /RESET pulse clears the control register to 0x00

#include "wokwi-api.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct {
  uint8_t channel_select;
} chip_state_t;

static bool on_i2c_connect(void *user_data, uint32_t address, bool read) {
  (void)user_data;
  (void)read;
  return address == 0x70;
}

static uint8_t on_i2c_read(void *user_data) {
  chip_state_t *chip = (chip_state_t *)user_data;
  return chip->channel_select;
}

static bool on_i2c_write(void *user_data, uint8_t data) {
  chip_state_t *chip = (chip_state_t *)user_data;
  chip->channel_select = data;
  printf("[TCA9548A] channel select = 0x%02x\n", data);
  return true;
}

static void on_i2c_disconnect(void *user_data) {
  (void)user_data;
}

void chip_init(void) {
  chip_state_t *chip = malloc(sizeof(chip_state_t));
  chip->channel_select = 0;

  pin_t pin_sda = pin_init("SDA", INPUT);
  pin_t pin_scl = pin_init("SCL", INPUT);

  i2c_config_t i2c_config = {
    .user_data = chip,
    .address = 0x70,
    .scl = pin_scl,
    .sda = pin_sda,
    .connect = on_i2c_connect,
    .read = on_i2c_read,
    .write = on_i2c_write,
    .disconnect = on_i2c_disconnect,
  };
  i2c_init(&i2c_config);

  printf("[TCA9548A] chip initialized at 0x70\n");
}
