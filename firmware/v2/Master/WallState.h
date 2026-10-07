#pragma once
// WallState.h — what the master knows about its Split-Flap as a whole
// (#559/#566): which boards it is made of and what each row board last said
// about itself. One mutex, snapshot copies: readers (web, MQTT) take a copy
// and never hold a pointer into it. Its mutex is a leaf: nothing is called
// while it is held.
//
// Writers: the link task publishes each row's link facts and unit facts. The
// rows table is loaded at start (NVS key "wallRows", WallRows.h's stored
// form) and changed only by the worker that runs an operator's request
// (WallPair.cpp): the web side stages a request here, it never changes the
// table itself. Jobs an operator started are WallOps.h.

#include <Arduino.h>

#include "SettingsStore.h"
#include "UnitFactsJson.h"
#include "WallOps.h"
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
  bool textApplied = false;   // the row reported the latest text as shown
  uint32_t shownCount = 0;    // texts the row reported shown, since this master started
  uint32_t lastLateMs = 0;    // how far past its instant the last one flipped (0 = on time)
  uint32_t worstLateMs = 0;   // the worst of them on the current connection
  uint8_t updateAttempts = 0; // failed offers of the stored image held against it
  bool updateBlocked = false; // given up on until something speaks for a fresh look
  wl_Status status = wl_Status_init_zero;
};

struct WallSnapshot {
  WallRowsTable rows;
  WallRowLink link[CLUSTER_MAX_MEMBERS];  // by index in `rows`
  // millis() when this table became the live one (0 = at start). Every
  // connection is dropped then, so a row's silence is counted from here.
  uint32_t rowsSinceMs = 0;
  // The offer of the stored row image that is out now (WallUpdatePolicy.h).
  uint8_t updatePhase = 0;  // WallUpdatePhase
  int8_t updateRow = -1;    // index in `rows`, -1 = none
};

// setup(), before tasksInit(): loads the rows table. A stored table that does
// not hold is not used (logged; the master runs on its own).
void wallStateInit(SettingsStore& store);

WallSnapshot wallStateGet();
// The table together with its generation, which changes whenever the table
// does: the link task drops every connection then, because rows are named by
// their index in it.
WallRowsTable wallStateRows(uint32_t& generation);
uint32_t wallStateRowsGeneration();

// Link task only.
void wallStatePublishLink(int row, const WallRowLink& link);
void wallStatePublishUnits(int row, const UnitFactsDoc& units, uint32_t nowMs);
void wallStatePublishUpdate(uint8_t phase, int row);

// A row board's unit facts as it last sent them. False when it has sent none
// since it was last welcomed. `atMs` is millis() at their arrival.
bool wallStateRowUnits(int row, UnitFactsDoc& out, uint32_t& atMs);

// ---- jobs ------------------------------------------------------------------------

// 0 when no job can be started now (WallOps.h).
uint32_t wallOpBegin(const char* name, int row);
// `board`: the id of the board the job was about, for a job that is not tied
// to a place in the rows table (a row being paired or released).
void wallOpFinish(uint32_t id, bool ok, const char* detail, const char* board = nullptr);
bool wallOpGet(uint32_t id, WallOp& out);
// Link task: a row came back with a new boot id.
void wallOpsFailRow(int row, const char* reason);

// ---- unit jobs -------------------------------------------------------------------
// One at a time per row. `row` is the index in the rows table, or
// WALL_OP_OWN_ROW for the master's own units.

// The largest result a job hands back: a unit's boot section.
#define WALL_OP_DATA_MAX 1024

// A job on a row: 0 when no job can be started now; `rowBusy` says whether
// that is because the row already runs one.
// `unit` is the bus address the job is about, 0 for a job on the whole row.
uint32_t wallJobBegin(const char* name, int row, bool& rowBusy, uint8_t unit = 0);
// What a job handed back besides its outcome, in the pieces it arrived in.
void wallOpDataPut(uint32_t id, uint32_t offset, const uint8_t* data, size_t n);
size_t wallOpDataGet(uint32_t id, uint8_t* out, size_t cap);

// Web side: hands a job for a row board to the link task, which writes it.
// `generation` is the rows table's the caller found `row` in. False when one
// is still waiting to be taken for that row, or the table has changed since.
bool wallJobStage(int row, const wl_Op& op, uint32_t generation);
// Link task, every pass.
bool wallJobTake(int row, wl_Op& out);

// The job on the master's own units whose end the worker is watching for
// (WallJobs.cpp): `seq` is its DisplayCommand's.
struct WallOwnJob {
  uint32_t opId = 0;  // 0 = none
  uint32_t seq = 0;
  uint32_t startedMs = 0;
  wl_Op op = wl_Op_init_zero;
};
void wallOwnJobSet(const WallOwnJob& job);
bool wallOwnJobGet(WallOwnJob& out);
void wallOwnJobClear(uint32_t opId);

// Is a job running on this row (an index in the rows table, or
// WALL_OP_OWN_ROW), and is it the update of its units?
bool wallJobRunningOn(int row);
bool wallUnitUpdateRunningOn(int row);

// Is a row board updating its units? The producer gate of the master's own
// unit update covers this too: nothing display-mutating starts meanwhile.
bool wallUnitUpdateRunning();

// ---- changing the rows table -----------------------------------------------------

enum class WallRequestKind : uint8_t { None, Pair, Release, Arrange };

// One operator request that changes the table. Staged by the web side, run by
// the worker; one at a time.
struct WallRequest {
  WallRequestKind kind = WallRequestKind::None;
  uint32_t opId = 0;
  char host[CLUSTER_HOST_MAX_LEN + 1] = {0};  // Pair: where the row is
  uint16_t port = 80;                         // Pair: its web port (a bench stand-in's differs)
  char id[WALL_ROW_ID_MAX + 1] = {0};         // Release: which row
  WallRowPlace place;                         // Pair
  WallRowsTable table;                        // Arrange: the whole new table
};

// False when another request is still waiting or running.
bool wallStateStage(const WallRequest& request);
// Worker: the staged request, if any. It stays "running" until
// wallStateRequestDone().
bool wallStateTakeRequest(WallRequest& out);
void wallStateRequestDone();

// Worker: judges the table, stores it and makes it the live one.
ClusterVerdict wallStateSetRows(const WallRowsTable& table);

// What every row board is told about how to behave (Config on the link): the
// time zone and whether it updates its units when it starts. Set from the
// settings at start and when they change; the link task sends it on.
void wallStateSetRowSettings(const String& tzPosix, bool updateUnitsAtStart);
// The settings and a number that changes whenever they do.
uint32_t wallStateRowSettings(wl_Config& out);
uint32_t wallStateRowSettingsGeneration();

// Worker asks, link task answers: say Release to this row if it is connected.
void wallStateAskRelease(const char* id);
bool wallStateReleaseAsked(char* idOut, size_t cap);
void wallStateReleaseAnswered();
bool wallStateReleasePending();

// Web side asks, link task takes: offer this row the stored image again,
// whatever was held against it. `generation` is the rows table's the caller
// found `row` in; false when the table has changed since.
bool wallStateAskUpdateRetry(int row, uint32_t generation);
// A bit per row index, cleared by the call.
uint32_t wallStateTakeUpdateRetries();

// Web side asks, link task takes: tell this row board to restart. The same
// rule for `generation`.
bool wallStateAskRestart(int row, uint32_t generation);
uint32_t wallStateTakeRestarts();
