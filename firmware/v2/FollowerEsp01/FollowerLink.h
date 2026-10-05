#pragma once
// FollowerLink.h — the row board's end of the wall link (#559/#564): one TCP
// connection to the master this row is paired with, opened and kept by the
// row. Messages are firmware/v2/link/wall_link.proto. loop() context only:
// what arrives is handed to the same cluster handlers the HTTP wire calls.

#include <stdint.h>

struct FollowerLinkView {
  bool connected = false;   // Welcome received on the current connection
  uint32_t connects = 0;    // connections that reached Welcome, since boot
  uint32_t drops = 0;       // connections lost or refused, since boot
};

void linkLoopTick();
FollowerLinkView linkViewGet();
