// Host harness for test_ota_gzip_eboot.py: the ESP8266 core's own eboot
// copy_raw() (pasted in from the installed core as copy_raw.inc) run over a
// simulated 1 MB flash. Reports whether the copy read any sector it had
// already overwritten — the way an in-place unpack would corrupt itself.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uzlib.h>

#define FLASH_SECTOR_SIZE 0x1000
#define FLASH_SIZE 0x100000

static uint8_t flash[FLASH_SIZE];
static uint8_t overwritten[FLASH_SIZE / FLASH_SECTOR_SIZE];
static int staleReads = 0;

int SPIRead(uint32_t addr, void* dst, size_t n) {
  if (addr + n > FLASH_SIZE) return 1;
  for (uint32_t s = addr / FLASH_SECTOR_SIZE;
       s <= (addr + n - 1) / FLASH_SECTOR_SIZE; s++) {
    if (overwritten[s]) staleReads++;
  }
  memcpy(dst, flash + addr, n);
  return 0;
}

int SPIEraseSector(uint32_t sector) {
  if (sector >= FLASH_SIZE / FLASH_SECTOR_SIZE) return 1;
  memset(flash + sector * FLASH_SECTOR_SIZE, 0xFF, FLASH_SECTOR_SIZE);
  overwritten[sector] = 1;
  return 0;
}

int SPIWrite(uint32_t addr, void* src, size_t n) {
  if (addr + n > FLASH_SIZE) return 1;
  memcpy(flash + addr, src, n);
  return 0;
}

void ets_putc(int c) { (void)c; }

#include "copy_raw.inc"

// eboot_harness <flash-in> <stage-addr> <upload-len> <flash-out>
int main(int argc, char** argv) {
  if (argc != 5) return 100;
  FILE* f = fopen(argv[1], "rb");
  if (!f || fread(flash, 1, FLASH_SIZE, f) != FLASH_SIZE) return 101;
  fclose(f);
  int res = copy_raw(strtoul(argv[2], 0, 0), 0, strtoul(argv[3], 0, 0), false);
  f = fopen(argv[4], "wb");
  if (!f || fwrite(flash, 1, FLASH_SIZE, f) != FLASH_SIZE) return 102;
  fclose(f);
  printf("res=%d staleReads=%d\n", res, staleReads);
  return 0;
}
