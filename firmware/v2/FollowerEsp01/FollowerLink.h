#pragma once
// FollowerLink.h — the row board's end of the wall link (#559/#564): one TCP
// connection to the master this row is paired with, opened and kept by the
// row. Messages are firmware/v2/link/wall_link.proto. Everything the master
// asks of this row arrives here: texts, settings, unit jobs, firmware, the
// log. loop() context only.

#include <stdint.h>

struct FollowerLinkView {
  bool connected = false;   // Welcome received on the current connection
  uint32_t connects = 0;    // connections that reached Welcome, since boot
  uint32_t drops = 0;       // connections lost or refused, since boot
};

void linkLoopTick();
FollowerLinkView linkViewGet();
