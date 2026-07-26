#pragma once
// ClusterLeaderInternal.h — shared seams of the ClusterLeader TU family
// (#352): ClusterLeader.cpp (statics + init + HTTP helpers + tick),
// ClusterLeaderGrid.cpp, ClusterLeaderFanout.cpp, ClusterLeaderRollout.cpp,
// ClusterLeaderFollowerPush.cpp, ClusterLeaderMaintenance.cpp,
// ClusterLeaderStatus.cpp. Include from these .cpp files ONLY — the
// WebEndpointsInternal.h rule; the public contract stays in ClusterLeader.h.
// Every variable here is guarded by leaderMutex unless its comment (at the
// definition in ClusterLeader.cpp) says otherwise.

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>

#include "ClusterLeader.h"
#include "ClusterRolloutPolicy.h"
#include "FollowerImagePolicy.h"
#include "SettingsStore.h"

#define CLUSTER_KEY_MEMBERS "clMembers"

extern SemaphoreHandle_t leaderMutex;

struct LeaderLock {
  LeaderLock() { xSemaphoreTake(leaderMutex, portMAX_DELAY); }
  ~LeaderLock() { xSemaphoreGive(leaderMutex); }
  LeaderLock(const LeaderLock&) = delete;
  LeaderLock& operator=(const LeaderLock&) = delete;
};

// Producer gate only — carries NO ordering for the mutex-guarded state;
// never read table/segments/runtimes off a bare enabledAtomic check.
extern std::atomic<bool> enabledAtomic;
extern String leaderDeviceName;
extern String leaderSelfRole;
extern String leaderTzPosix;
extern SettingsStore* leaderStore;

extern ClusterMemberTable table;
extern ClusterMemberRuntime runtimes[CLUSTER_MAX_MEMBERS];
extern String segments[CLUSTER_MAX_MEMBERS];
extern uint32_t epoch;
extern uint32_t seqCounter;
extern int gridSpeed;
extern String gridAlignment;
extern uint64_t gridCommitAtMs;
extern String lastContentKey;
extern bool selfPending;
extern String selfText;
extern int selfSpeed;
// The producer behind selfText (#403), carried across the grid boundary so
// the leader's own row credits the browser/MQTT/clock rather than itself.
extern DisplaySource selfSource;
extern uint32_t selfDueMs;
extern bool configPending;
extern String configSpec;
extern bool configSuppressLeave;
extern uint32_t digestGen;
extern String digestLastBody;
extern ClusterRolloutState rollout;
extern FollowerPushState followerPush;
extern bool followerPushPending;
extern String followerPushHost;
extern std::atomic<uint32_t> gridGenerationAtomic;

// #321 reboot-hold delivery (built by Maintenance, drained by the tick).
struct RebootHoldTarget {
  String host;
  String body;
};
extern RebootHoldTarget rebootHoldTargets[CLUSTER_MAX_MEMBERS];
extern int rebootHoldCount;
extern int rebootHoldCursor;
extern uint32_t rebootHoldMs;
extern std::atomic<bool> rebootHoldPending;
extern std::atomic<bool> rebootHoldSent;

// Rollout seams shared across Rollout/FollowerPush/Status (#276/#344/#304);
// the flash/file session handles stay TU-private.
extern std::atomic<bool> rolloutFactsFailed;
extern uint8_t rolloutBuf[4096];
extern bool rolloutFollowerSource;
extern String rolloutFollowerTargetRev;
extern bool rolloutRescueTriggered;

// One outbound fan-out call (#320) — built under the lock, sent unlocked.
struct MemberWorkItem {
  int index = -1;
  ClusterLeaderAction action = ClusterLeaderAction::None;
  String url;
  String body;
};

// ClusterLeader.cpp (core)
uint64_t epochNowMs(bool& synced);
void clusterMintKey(uint8_t key[32]);
String urlEncode(const String& value);
int clusterHttpRequest(const String& url, const String& postBody,
                       String& outBody);
int clusterHttpGetBody(const String& url, String& outBody);

// ClusterLeaderGrid.cpp
void serviceSelfRow();

// ClusterLeaderFanout.cpp
void applyStagedConfig();
int collectMemberWork(MemberWorkItem* items);
void applyMemberResult(const MemberWorkItem& item, int status,
                       const String& body);

// ClusterLeaderRollout.cpp
void rolloutServiceTick();
void rolloutCloseUpload();

// ClusterLeaderFollowerPush.cpp
void followerPushServiceTick();
void followerPushCloseUpload();
void followerPushPump();
void rolloutTryFollowerImageStart();

// ClusterLeaderMaintenance.cpp
void rebootHoldBuildTargets();
void followerLogPullTick();

// ClusterLeaderStatus.cpp
void statusFillLocked(ClusterLeaderStatus& st);
// #294/#321 digest build, deduped from collectMemberWork +
// rebootHoldBuildTargets (#355) — leaderMutex HELD; bumps digestGen only
// when the piggybacked content actually changed.
String buildFreshDigestLocked(const String& leaderMode);
