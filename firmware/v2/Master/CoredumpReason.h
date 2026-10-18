#pragma once
#include <stddef.h>

// What the panic handler wrote into the coredump on flash (#630): for a task
// watchdog reset, the task that stopped feeding. Given on one line, and only
// from a dump this build wrote — a dump another build left says nothing about
// this boot (the summary route still serves it, marked stale).
// False when there is no dump, it is another build's, or it holds no reason.
bool coredumpPanicReason(char* out, size_t cap);
