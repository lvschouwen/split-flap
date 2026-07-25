// ClusterLeaderFanout.cpp — config swap + member work collection/result fold (#273/#320)
// Split out of ClusterLeader.cpp (#352); contract in ClusterLeader.h,
// shared seams in ClusterLeaderInternal.h.

#include "ClusterLeader.h"

#include <LittleFS.h>
#include <MD5Builder.h>
#include <WiFi.h>
#include <esp_http_client.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <errno.h>  // #340: lwip socket errno for stream-failure diagnostics
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sys/time.h>

#include <atomic>

#include "BuildVersion.h"  // GIT_REV — the cluster's firmware version (#276)
#include "ClockPolicy.h"  // clockIsTimeSynced
#include "ClusterDigest.h"  // ping piggyback: digest build + health parse (#294)
#include "ClusterFollowerPolicy.h"  // clusterRenderDelayMs (shared math)
#include "ClusterHmac.h"  // cluster-wire auth: key mint + request signing
#include "WearPolicy.h"  // self-row wear fold for the status/digest health
#include "ClusterRolloutPolicy.h"
#include "DisplayCommand.h"
#include "FollowerImagePolicy.h"  // #304 on-demand esp01 firmware relay
#include "FollowerImageStore.h"
#include "HelpersSerialHandling.h"
#include "MqttService.h"  // mqttNotificationActive — self-row re-show gate
#include "ReflashPlan.h"
#include "Tasks.h"
#include "TaskWatchdog.h"
#include "WebBodyLimit.h"  // #386: ping digest budget — same ceiling the guard enforces
#include "WebEndpoints.h"  // #337: webDisplayContentSnapshot() — leader's mode

#include "ClusterLeaderInternal.h"

// --- clusterTask body -----------------------------------------------------------

// Applies a staged member-table swap: leave fan-out to dropped remote
// hosts (single best-effort attempt), runtime/segment reset, NVS persist.
// #385 render-stuck log latch: one onset + one clear line per episode
// (transition-only logging); reset with the runtimes on every config apply.
static bool renderStuckLogged[CLUSTER_MAX_MEMBERS] = {false};

void applyStagedConfig() {
  String spec;
  bool suppressLeave;
  {
    LeaderLock lock;
    if (!configPending) return;
    configPending = false;
    spec = configSpec;
    suppressLeave = configSuppressLeave;
    configSuppressLeave = false;
  }

  ClusterMemberTable next;
  ClusterGrid grid;
  bool nextEnabled = clusterTableFromString(spec, next) && next.count > 0 &&
                     validateMemberTable(next, grid).ok;
  if (!nextEnabled && spec.length() > 0) {
    // The web boundary validated this same spec — a failure here means it
    // was clobbered in between; refuse rather than half-apply.
    SerialPrintln(F("cluster: staged config no longer valid — ignored"));
    return;
  }

  // A config swap invalidates any in-flight rollout (member indexes move);
  // same task as the upload pump, so the teardown is race-free (#276). The
  // on-demand follower push (#304) is torn down the same way.
  rolloutCloseUpload();
  followerPushCloseUpload();

  // Old remote hosts that are not in the new table get a leave.
  String leaveHosts[CLUSTER_MAX_MEMBERS];
  uint8_t leaveKeys[CLUSTER_MAX_MEMBERS][32];
  bool leaveKeyValid[CLUSTER_MAX_MEMBERS];
  int leaveCount = 0;
  {
    LeaderLock lock;
    for (int i = 0; i < table.count; i++) {
      if (clusterMemberIsSelf(table.members[i])) continue;
      bool kept = false;
      for (int j = 0; j < next.count; j++) {
        if (strcmp(table.members[i].host, next.members[j].host) == 0) {
          kept = true;
          break;
        }
      }
      if (!kept && !suppressLeave) {
        // Capture the member's auth key BEFORE the runtime reset below so the
        // leave can be signed (#313 follow-on) — a keyed follower requires it.
        leaveKeyValid[leaveCount] = runtimes[i].hmacKeyValid;
        if (runtimes[i].hmacKeyValid) {
          memcpy(leaveKeys[leaveCount], runtimes[i].hmacKey, 32);
        }
        leaveHosts[leaveCount++] = table.members[i].host;
      }
    }
    table = next;
    for (int i = 0; i < CLUSTER_MAX_MEMBERS; i++) {
      runtimes[i] = ClusterMemberRuntime{};
      // #385 benefit-of-the-doubt epoch: a fresh table entry gets the full
      // 30 s silence window before a failure can degrade it.
      clusterMemberStampContactEpoch(runtimes[i], millis());
      segments[i] = "";
      renderStuckLogged[i] = false;
    }
    clusterRolloutReset(rollout);
    followerPush = FollowerPushState{};
    followerPushPending = false;
    lastContentKey = "";  // force a fresh submit → renders for everyone
    selfPending = false;
    selfText = "";
    enabledAtomic.store(nextEnabled);
    gridGenerationAtomic.fetch_add(1, std::memory_order_relaxed);
  }

  // NVS write from clusterTask: safe — the NVS layer is internally
  // mutex-protected, and the netTask-sole-flash-writer Hard rule governs
  // the storage LittleFS partition, not the nvs partition.
  leaderStore->putString(CLUSTER_KEY_MEMBERS, nextEnabled ? spec : String(""));
  SerialPrintf("cluster: config applied — %s, %d member(s)\n",
               nextEnabled ? "leading" : "disabled", (int)next.count);

  for (int i = 0; i < leaveCount; i++) {
    String body;
    // Sign the leave (#313 follow-on) so a keyed follower honors it; an
    // un-keyed (pre-HMAC) follower ignores the extra params and falls back to
    // #313 source-IP acceptance.
    String post;
    if (leaveKeyValid[i]) {
      bool synced = false;
      uint64_t ts = epochNowMs(synced);
      String msg = clusterHmacLeaveMsg(ts);
      post = "ts=" + clusterU64ToStr(ts) + "&mac=" +
             clusterHmacSign(leaveKeys[i], msg);
    }
    clusterHttpRequest("http://" + leaveHosts[i] + "/cluster/leave", post,
                       body);
    SerialPrintln("cluster: sent leave to " + leaveHosts[i]);
  }
}

// Round-robin cursor for the one-op-per-tick fan-out (#320). clusterTask-
// private: only collectMemberWork touches it, always under LeaderLock.
static int fanoutCursor = -1;

// Builds the next round of outbound calls under the lock; the blocking
// HTTP happens with the mutex RELEASED so submits/status reads never wait
// on a slow follower. At most ONE member is contacted per tick (#320): a
// promote-time catch-up makes every member due at once, and doing them all in
// one tick — one being a dead host burning the full HTTP timeout — starved
// core-0 idle past the 5 s task watchdog. clusterFanoutNext round-robins so a
// stuck dead host can't monopolize the fan-out.
int collectMemberWork(MemberWorkItem* items) {
  // #337: capture the leader's mode for the digest BEFORE taking LeaderLock.
  // webDisplayContentSnapshot() takes WebStateLock, and the documented order is
  // WebStateLock -> clusterMutex — nesting it the other way deadlocks against
  // netTask's settings drain (WebStateLock -> clusterLeaderSubmit*).
  String leaderMode = webDisplayContentSnapshot().deviceMode;
  LeaderLock lock;
  int count = 0;
  uint32_t nowMs = millis();
  // One epoch-ms stamp for every signed request this round (#313 follow-on).
  bool signSynced = false;
  uint64_t signTs = epochNowMs(signSynced);

  bool needsAction[CLUSTER_MAX_MEMBERS] = {false};
  for (int i = 0; i < table.count; i++) {
    if (clusterMemberIsSelf(table.members[i])) continue;
    // While a rollout upload streams to a member, its flash writes hog the
    // follower's async_tcp task — regular contact would time out and read
    // as failures. Skip it; the upload round-trip IS the contact (#276), so
    // it must also COUNT as contact (#385): hold the epoch fresh while we
    // deliberately aren't probing, or a >30 s upload leaves a stale contact
    // age that degrades the member on its first post-upload timeout —
    // whichever way the upload window ends, supervision restarts with the
    // full silence window.
    if (rollout.phase == ClusterRolloutPhase::Uploading &&
        rollout.memberIndex == i) {
      clusterMemberStampContactEpoch(runtimes[i], nowMs);
      continue;
    }
    // Same hazard for the on-demand follower-image push (#304): don't ping a
    // row whose async_tcp task is busy taking our /firmware/master stream.
    if (followerPush.phase == FollowerPushPhase::Uploading &&
        followerPush.memberIndex == i) {
      clusterMemberStampContactEpoch(runtimes[i], nowMs);
      continue;
    }
    needsAction[i] =
        clusterMemberNextAction(runtimes[i], nowMs) != ClusterLeaderAction::None;
  }

  int pick = clusterFanoutNext(fanoutCursor, needsAction, table.count);
  if (pick >= 0) {
    const ClusterMemberDef& def = table.members[pick];
    ClusterLeaderAction action = clusterMemberNextAction(runtimes[pick], nowMs);
    MemberWorkItem& item = items[count++];
    item.index = pick;
    item.action = action;
    String host(def.host);
    switch (action) {
      case ClusterLeaderAction::Join: {
        // Resolved only when a join is actually due — a per-tick
        // WiFi.localIP().toString() would churn the heap ~10x/s for a
        // value that only changes on DHCP renewal.
        item.url = "http://" + host + "/cluster/join";
        // Mint the per-member wire-auth key once (#313 follow-on); reused
        // across rejoins so the key stays stable while the membership does.
        if (!runtimes[pick].hmacKeyValid) {
          clusterMintKey(runtimes[pick].hmacKey);
          runtimes[pick].hmacKeyValid = true;
        }
        item.body = "leaderName=" + urlEncode(leaderDeviceName) +
                    "&leaderHost=" + urlEncode(WiFi.localIP().toString()) +
                    "&row=" + String((int)def.row) +
                    "&epoch=" + String((unsigned long)epoch) +
                    "&key=" + clusterKeyToHex(runtimes[pick].hmacKey);
        if (leaderTzPosix.length() > 0) {
          // #342 additive: pre-#342 followers ignore unknown params.
          item.body += "&tz=" + urlEncode(leaderTzPosix);
        }
        break;
      }
      case ClusterLeaderAction::Render: {
        // Seq is minted per POST attempt: an applied-but-timed-out render
        // retries with a higher seq and re-applies IDENTICAL content (a
        // visual no-op), while cross-ordered stale renders still lose the
        // seq race. A stable per-content seq would save that no-op but
        // couple retry state to content state for no wall-visible gain.
        item.url = "http://" + host + "/cluster/render";
        uint32_t seq = ++seqCounter;
        item.body = "epoch=" + String((unsigned long)epoch) +
                    "&seq=" + String((unsigned long)seq) +
                    "&text=" + urlEncode(segments[pick]) +
                    "&speed=" + String(gridSpeed) +
                    "&commitAtMs=" + String((unsigned long long)gridCommitAtMs);
        // Sign the content (#313 follow-on) once a key is negotiated — the
        // canonical fields mirror what the follower reconstructs from the
        // wire params, so a captured mac can't be reused for other text.
        if (runtimes[pick].hmacKeyValid) {
          String msg = clusterHmacRenderMsg(signTs, epoch, seq, segments[pick],
                                            gridSpeed, gridCommitAtMs);
          item.body += "&ts=" + clusterU64ToStr(signTs) + "&mac=" +
                       clusterHmacSign(runtimes[pick].hmacKey, msg);
        }
        break;
      }
      default:  // Ping
        item.url = "http://" + host + "/cluster/ping";
        item.body = "";  // digest piggyback attached below, once per round
        break;
    }
  }

  // #294 rung 2: the digest rides every outbound ping — built at most once
  // per round (~10 s cadence per member), gen-bumped only on real change.
  bool anyPing = false;
  for (int i = 0; i < count; i++) {
    if (items[i].action == ClusterLeaderAction::Ping) anyPing = true;
  }
  if (anyPing) {
    String digest = buildFreshDigestLocked(leaderMode);
    String encoded = urlEncode(digest);
    // #386: the digest is an OPTIONAL piggyback (#294 — absent ⇒ ""), the
    // liveness ping is NOT. URL-encoding inflates a JSON digest ~65%, so a
    // wall that grows past the ceiling used to 413 every ping and degrade the
    // whole cluster. Drop the digest instead: the wall mirror goes stale, the
    // cluster stays alive. Clearing `digest` too keeps the signed canonical
    // equal to what is actually sent (both follower copies reconstruct "" from
    // an absent param).
    // Overhead = "you=" + index + "&ts=" + 13 + "&mac=" + 64 + separators.
    const size_t kPingOverheadBytes = 128;
    const bool sendDigest =
        pingBodyDigestFits(encoded.length(), kPingOverheadBytes);
    // Transition-only, BOTH edges (#322 philosophy): a wall that grows past
    // the budget and one that shrinks back under it. clusterTask is the sole
    // caller (ClusterLeader.cpp), so the function-local static is safe.
    static bool digestOmitted = false;
    if (!sendDigest && !digestOmitted) {
      digestOmitted = true;
      SerialPrintln("cluster: ping digest too large (" +
                    String(encoded.length()) + "B encoded + overhead > " +
                    String((unsigned long)kMaxPingBodyBytes) +
                    "B budget) — pinging without it; wall mirror will lag");
    } else if (sendDigest && digestOmitted) {
      digestOmitted = false;
      SerialPrintln("cluster: ping digest fits again (" +
                    String(encoded.length()) + "B encoded) — wall mirror live");
    }
    if (!sendDigest) {
      digest = "";
      encoded = "";
    }
    for (int i = 0; i < count; i++) {
      if (items[i].action != ClusterLeaderAction::Ping) continue;
      items[i].body = sendDigest
                          ? ("digest=" + encoded + "&you=" +
                             String(items[i].index))
                          : ("you=" + String(items[i].index));
      // Sign the ping per member (#313 follow-on): its own key, so the
      // digest/you piggyback rides an authenticated request.
      int mi = items[i].index;
      if (runtimes[mi].hmacKeyValid) {
        // Sign the raw (un-encoded) digest + this member's index too (#313
        // follow-on HIGH#2): the follower persists them as promote-critical
        // state, so the mac must bind them against an on-path swap.
        String msg = clusterHmacPingMsg(signTs, digest, mi);
        items[i].body += "&ts=" + clusterU64ToStr(signTs) + "&mac=" +
                         clusterHmacSign(runtimes[mi].hmacKey, msg);
      }
#if CLUSTER_WIRE_DEBUG
      // #386: the ping body is the one request whose size scales with member
      // count (it carries the whole cluster digest). kMaxNonUploadBodyBytes is
      // 2048 — measure, do not assume.
      SerialPrintln("dbg/wire: ping body idx=" + String(mi) + " digestRaw=" +
                    String(digest.length()) + "B encoded=" +
                    String(encoded.length()) + "B total=" +
                    String(items[i].body.length()) + "B keyed=" +
                    String(runtimes[mi].hmacKeyValid ? 1 : 0));
#endif
    }
  }
  return count;
}

void applyMemberResult(const MemberWorkItem& item, int status,
                       const String& body) {
  LeaderLock lock;
  ClusterMemberRuntime& m = runtimes[item.index];
  uint32_t nowMs = millis();

#if CLUSTER_WIRE_DEBUG
  // #386 bench trace: every contact result, including the ones the normal
  // path swallows. status -1 = transport failure (connect/write/timeout);
  // 403 = member rejected the signature; 413 = body guard tripped before the
  // handler ran. Without this the leader records only "contact failed".
  {
    const char* actionName =
        item.action == ClusterLeaderAction::Join     ? "JOIN"
        : item.action == ClusterLeaderAction::Render ? "RENDER"
        : item.action == ClusterLeaderAction::Ping   ? "PING"
                                                     : "NONE";
    String replyHead = body.substring(0, body.length() > 96 ? 96 : body.length());
    replyHead.replace('\n', ' ');
    SerialPrintln("dbg/wire: " + String(table.members[item.index].host) + " " +
                  actionName + " status=" + String(status) + " req=" +
                  String(item.body.length()) + "B reply=" +
                  String(body.length()) + "B \"" + replyHead + "\"");
  }
#endif

  if (status == 200) {
    bool wasDegraded = m.degraded;
    bool wasSuspect = clusterMemberSuspect(m);
    clusterMemberOnSuccess(m, nowMs);
    switch (item.action) {
      case ClusterLeaderAction::Join: {
        m.joined = true;
        // #343: a fresh join means the member was just down or reclaimed —
        // the continuous-healthy window restarts here.
        m.healthySinceMs = nowMs;
        String joinRev = clusterExtractJsonString(body, "rev");
        // #340: a rev change (manual OTA, bench flash) forgives a rollout
        // give-up — the blocked latch was pinned to a build that's gone.
        clusterRolloutNoteMemberRev(rollout, item.index, m.rev, joinRev);
        m.rev = joinRev;
        // Absent = same platform as this leader (#297); an ESP-01 row
        // reports "esp01" and is excluded from firmware convergence.
        m.plat = clusterExtractJsonString(body, "plat");
        // Absent = pre-#332 peer. NOT the same as an explicit "display":
        // "" keeps the old width-0-preferred slot (tier 0), "display" at
        // width 0 ranks with spare — so a mixed-rev cluster is unchanged.
        m.role = clusterExtractJsonString(body, "role");
        // #343 additive: rescue:1 = the member boots as a rescue beacon and
        // wants the stored follower image re-pushed. Absent = healthy.
        m.rescue = clusterExtractJsonInt(body, "rescue", 0) != 0;
        m.reportedWidth = clusterExtractJsonInt(body, "width", 0);
        // The handshake ends with a re-send of the current segment.
        if (segments[item.index].length() > 0) {
          clusterMemberMarkRenderDirty(m, nowMs);
        } else {
          clusterMemberRenderAcked(m);
        }
        SerialPrintln("cluster: member " +
                      String(table.members[item.index].host) + " joined (rev " +
                      m.rev + (m.hmacKeyValid ? ", authenticated)" : ")"));
        break;
      }
      case ClusterLeaderAction::Render:
        clusterMemberRenderAcked(m);
        if (renderStuckLogged[item.index]) {
          renderStuckLogged[item.index] = false;
          SerialPrintln("cluster: member " +
                        String(table.members[item.index].host) +
                        " render unstuck (segment delivered)");
        }
        break;
      default:
        break;
    }
    if (item.action != ClusterLeaderAction::Render) {
      // Join and ping replies carry the #294 health keys; a pre-#294
      // follower parses to invalid (strip hidden, never zero faults). The
      // rev refresh keeps the fleet-convergence fact alive across OUR
      // reboots without waiting for a re-join.
      clusterParsePingHealth(body, m.health);
      String rev = clusterExtractJsonString(body, "rev");
      if (rev.length() > 0) {
        // #340: same forgiveness on the ping-refresh path — this is how a
        // stale updateBlocked clears after the member converges without us.
        clusterRolloutNoteMemberRev(rollout, item.index, m.rev, rev);
        m.rev = rev;
      }
      String plat = clusterExtractJsonString(body, "plat");
      if (plat.length() > 0) m.plat = plat;
      // #332: ping refresh keeps a live role change (role picker POST on the
      // member) flowing into the succession tiers without a re-join.
      String role = clusterExtractJsonString(body, "role");
      if (role.length() > 0) m.role = role;
      // #343: absolute on every reply — a healed boot stops beaconing.
      m.rescue = clusterExtractJsonInt(body, "rescue", 0) != 0;
      if (m.rescue) m.healthySinceMs = nowMs;  // beaconing ≠ healthy
      m.reportedWidth = clusterExtractJsonInt(body, "width", m.reportedWidth);
    }
    if (wasDegraded) {
      SerialPrintln("cluster: member " +
                    String(table.members[item.index].host) + " recovered");
    } else if (wasSuspect) {
      SerialPrintln("cluster: member " +
                    String(table.members[item.index].host) +
                    " suspect cleared");
    }
    return;
  }

  if (status == 409) {
    // The follower answered but rejected: for ping/render that means it
    // lost its membership — fresh join next round, no backoff (the link
    // is fine).
    if (item.action != ClusterLeaderAction::Join) {
      bool wasDegraded = m.degraded;
      bool wasSuspect = clusterMemberSuspect(m);
      clusterMemberOnSuccess(m, nowMs);
      clusterMemberOnNotClustered(m);
      // #385: same transition-only logging as the 200 path — the member
      // answered, so a suspect/degraded episode ends here.
      if (wasDegraded) {
        SerialPrintln("cluster: member " +
                      String(table.members[item.index].host) +
                      " recovered (answering, re-joining)");
      } else if (wasSuspect) {
        SerialPrintln("cluster: member " +
                      String(table.members[item.index].host) +
                      " suspect cleared (answering, re-joining)");
      }
      return;
    }
    // A join 409 with the other-leader marker (#295 sticky leadership):
    // the member belongs to a promoted successor — WE are the stale
    // leader. Demote: stage a table wipe with the leave fan-out
    // suppressed (the members are the new leader's to keep).
    if (clusterJoinRejectedOtherLeader(body)) {
      SerialPrintln("cluster: member " +
                    String(table.members[item.index].host) +
                    " is clustered to another live leader (" +
                    clusterExtractJsonString(body, "leaderHost") +
                    ") — DEMOTING, this board joins the wall as a member");
      configSpec = "";
      configPending = true;
      configSuppressLeave = true;
      return;
    }
  }

  bool wasDegraded = m.degraded;
  bool wasSuspect = clusterMemberSuspect(m);
  // #385 leader-offline gate: while our own STA is down, failures are the
  // leader's problem, not member evidence (clusterLeaderTick re-stamps every
  // member's contact epoch on the up-edge).
  clusterMemberOnFailure(m, nowMs, WiFi.status() == WL_CONNECTED);
  if (m.degraded && !wasDegraded) {
    SerialPrintln("cluster: member " + String(table.members[item.index].host) +
                  " DEGRADED (no re-layout; its board falls back alone)");
  } else if (clusterMemberSuspect(m) && !wasSuspect) {
    SerialPrintln("cluster: member " + String(table.members[item.index].host) +
                  " suspect (contact failed — retrying, membership kept)");
  }
  if (clusterMemberRenderStuck(m, nowMs) && !renderStuckLogged[item.index]) {
    renderStuckLogged[item.index] = true;
    SerialPrintln("cluster: member " + String(table.members[item.index].host) +
                  " render STUCK (alive, but segment undelivered for 30 s)");
  }
}
