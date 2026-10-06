#pragma once
// WallState.h — what the master knows about its Split-Flap as a whole
// (#559/#566): which boards it is made of and what each row board last said
// about itself. One mutex, snapshot copies: readers (web, MQTT) take a copy
// and never hold a pointer into it. Its mutex is a leaf: nothing is called
// while it is held.
//
// Writers: the link task publishes each row's link facts; the rows table is
// loaded at start (NVS key "wallRows", WallRows.h's stored form).

#include <Arduino.h>

#include "SettingsStore.h"
#include "WallLinkPolicy.h"
#include "WallRows.h"

#define WALL_ROWS_NVS_KEY "wallRows"

// What the link knows about one row board. Empty for the master's own row.
struct WallRowLink {
  WallRowContact contact;
  bool everWelcomed = false;
  bool rescue = false;
  uint8_t reportedWidth = 0;  // units the row found, from its Hello
  char rev[sizeof(((wl_Hello*)0)->rev)] = {0};
  char address[16] = {0};     // where its connection comes from
  uint32_t connects = 0;      // connections welcomed since this master started
  uint32_t restarts = 0;      // of the row, seen as a new boot id
  bool haveStatus = false;    // on the current connection
  wl_Status status = wl_Status_init_zero;
};

struct WallSnapshot {
  WallRowsTable rows;
  WallRowLink link[CLUSTER_MAX_MEMBERS];  // by index in `rows`
};

// setup(), before tasksInit(): loads the rows table. A stored table that does
// not hold is not used (logged; the master runs on its own).
void wallStateInit(SettingsStore& store);

WallSnapshot wallStateGet();
WallRowsTable wallStateRows();
// Changes whenever the rows table does; the link task drops every connection
// then, because rows are named by their index in it.
uint32_t wallStateRowsGeneration();

// Link task only.
void wallStatePublishLink(int row, const WallRowLink& link);
