#pragma once
// #542: crash record clear for the unit application.
// twiboot reserves 0x08FE/0x08FF above both stacks (--defsym=__stack=0x8FD)
// and counts consecutive WDT resets there. Writing count=0 tells the
// bootloader the application is healthy or the reset is intentional.

#include <avr/io.h>
#include <stdint.h>

#define CRASH_REC_A   (RAMEND - 1)
#define CRASH_REC_TAG 0xA0

static inline void crashRecordClear(void) {
    *(volatile uint8_t *)(CRASH_REC_A)     = CRASH_REC_TAG;
    *(volatile uint8_t *)(CRASH_REC_A + 1) = (uint8_t)~CRASH_REC_TAG;
}
