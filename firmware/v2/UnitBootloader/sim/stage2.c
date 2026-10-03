#include <avr/io.h>
#include <stdint.h>
typedef void (*spm_fn)(uint16_t addr, uint8_t action, uint16_t data);
#define DO_SPM ((spm_fn)(0x7F80 >> 1))   /* AVR fn pointers are word addresses */
#define TARGET 0x7F00
volatile uint8_t done;
int main(void) {
  SP = RAMEND;
  DO_SPM(TARGET, (1<<PGERS)|(1<<SELFPRGEN), 0);          /* erase */
  for (uint8_t i = 0; i < 64; i++) {
    uint16_t w = (uint16_t)(0xC0 + 2*i) | ((uint16_t)(0xC0 + 2*i + 1) << 8);
    DO_SPM(TARGET + 2*i, (1<<SELFPRGEN), w);             /* fill */
  }
  DO_SPM(TARGET, (1<<PGWRT)|(1<<SELFPRGEN), 0);          /* write */
  DO_SPM(TARGET, (1<<RWWSRE)|(1<<SELFPRGEN), 0);         /* rww enable */
  done = 0xFF;
  for (;;) {}
}
