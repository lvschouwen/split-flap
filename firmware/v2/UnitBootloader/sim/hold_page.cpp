// Writes the hold page exactly as a flash sends it (shared/TwibootFlash.h),
// so the proof runs the bytes the boards use and not a copy of them.
#include <cstdio>

#include "TwibootFlash.h"

int main() {
  uint8_t page[TWIBOOT_PAGE_SIZE];
  twibootFillHoldPage(page);
  return fwrite(page, 1, sizeof(page), stdout) == sizeof(page) ? 0 : 1;
}
