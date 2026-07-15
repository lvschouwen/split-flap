#pragma once
// FollowerImageStore — the S3 side of the ESP-01 follower-image relay
// (#304 Part B). Holds ONE staged follower-*.bin on the shared `storage`
// LittleFS (/follower-fw.bin + /follower-fw.rev) so the leader can stream it
// to esp01 rows on demand (ClusterLeader.cpp), reusing #276's machinery but
// sourcing the file instead of the running slot.
//
// Writer discipline (Hard rule: netTask is the SOLE `storage` flash writer):
// the async upload handler accumulates the whole image into a PSRAM buffer
// and verifies its MD5; the actual file write happens in
// followerImageFlushTick() on netTask. The relay READS the file from
// clusterTask — the same cross-task LittleFS read the /log/flash handler
// already relies on (esp_littlefs VFS lock). Upload and relay are mutually
// exclusive so the file can't be rewritten mid-stream.
//
// Pure decisions (filename guard, chunk cursor/bounds) live in
// FollowerImagePolicy.h; this module is the LittleFS + MD5 glue (bench-tier).

#include <Arduino.h>

// Absolute LittleFS paths.
#define FOLLOWER_IMAGE_PATH "/follower-fw.bin"
#define FOLLOWER_IMAGE_REV_PATH "/follower-fw.rev"

// PSRAM accumulation ceiling — well over a real ~384 KB follower image, well
// under the ESP-01's own sketch space, so an absurd upload is refused early.
static const size_t FOLLOWER_IMAGE_MAX_BYTES = 640 * 1024;

// Setup context: read the stored rev + presence off flash (LittleFS already
// mounted by flashLogInit()). Idempotent.
void followerImageStoreInit();

// Stored-image queries (leaderTask eligibility, web status). Thread-safe.
bool followerImageStored();
String followerImageStoredRev();

// Atomically claim the file for a relay stream: fails (returns false) if the
// accumulator is mid-upload or a write is pending to flush — i.e. the file is
// stale or about to change. On success sets the relay-active flag so the
// netTask flush defers until followerImageReleaseRelay(). Checking "busy" and
// setting the flag must be ONE critical section, else a writeEnd can slip a
// pending flush in between and race the relay's read (esp_littlefs serializes
// each VFS call, not a whole multi-tick read/write session).
bool followerImageTryClaimRelay();

// Release the relay claim (every terminal path of a push MUST call this — a
// stuck flag wedges all future uploads' flushes).
void followerImageReleaseRelay();

// --- upload accumulator (async_tcp handler context) ---------------------------------
// Same deliberate async-context exception as Update.write in /firmware/master:
// a firmware stream can't be queued chunk-by-chunk. But it writes to a PSRAM
// buffer, NOT flash — the flash write is deferred to netTask (below).

// begin: refuses (returns false, sets error) if another upload is already
// accumulating or its flush is still pending; allocates the PSRAM buffer,
// stores the expected md5 + rev. A concurrent relay stream is fine — the new
// image just accumulates in PSRAM and its flush defers behind the relay.
bool followerImageWriteBegin(const String& expectedMd5, const String& rev);
// chunk: cursor-matched append (FollowerImagePolicy.h).
bool followerImageWriteChunk(const uint8_t* data, size_t len, size_t streamOffset);
// end: MD5-verifies the buffer; on success stages it for netTask to write and
// returns true. On any failure returns false (error set, buffer freed).
bool followerImageWriteEnd();
// "" while the last begin/chunk/end sequence has no error.
String followerImageWriteError();

// --- netTask flush ------------------------------------------------------------------
// Writes the staged buffer to FOLLOWER_IMAGE_PATH + FOLLOWER_IMAGE_REV_PATH
// (one open→write→close each), updates the stored-rev/presence state, frees
// the buffer. No-op when nothing is staged. Call every netTask loop.
void followerImageFlushTick();
