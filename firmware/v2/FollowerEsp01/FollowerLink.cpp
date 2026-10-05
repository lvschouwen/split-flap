// FollowerLink.cpp — socket glue of the wall link on the row board. Timing
// rules in FollowerLinkPolicy.h; message handling reuses FollowerCluster's
// handlers, so the phase machine, flip timing and fallback clock are the ones
// the HTTP wire already drives.
#include "FollowerLink.h"

#include <Arduino.h>
#include <ESP8266WiFi.h>

#include "BuildVersion.h"
#include "FollowerBus.h"
#include "FollowerBusRecovery.h"
#include "FollowerCluster.h"
#include "FollowerEscalation.h"
#include "FollowerLinkPolicy.h"
#include "FollowerRescue.h"
#include "FollowerWeb.h"
#include "FollowerWifi.h"
#include "WallLinkStream.h"

namespace {

WiFiClient sock;
WlRowReader reader;
// One outgoing message at a time. Static: wl_ToMaster holds the largest
// payload (480 B) and loop()'s stack is 4 KB.
wl_ToMaster out;
uint8_t wire[WL_PREFIX_MAX + wl_ToMaster_size];

bool welcomed = false;
uint32_t bootId = 0;
uint32_t connectCount = 0;
uint32_t dropCount = 0;
uint32_t backoffMs = 0;
uint32_t nextDialMs = 0;
uint32_t connectedAtMs = 0;
uint32_t lastStatusMs = 0;

bool send() {
  const size_t n = wlEncodeToMaster(wire, sizeof(wire), out);
  return n != 0 && sock.write(wire, n) == n;
}

void drop(const __FlashStringHelper* why) {
  if (sock.connected() || welcomed) {
    SerialPrint(F("link: closed — "));
    SerialPrintln(why);
  }
  sock.stop();
  reader.reset();
  welcomed = false;
  dropCount++;
  backoffMs = followerLinkNextBackoffMs(backoffMs);
  nextDialMs = millis() + backoffMs;
}

void sendHello() {
  wlClear(out);
  out.which_body = wl_ToMaster_hello_tag;
  wl_Hello& h = out.body.hello;
  h.protocol = WALL_LINK_PROTOCOL;
  strlcpy(h.id, effectiveDeviceName.c_str(), sizeof(h.id));
  strlcpy(h.rev, GIT_REV, sizeof(h.rev));
  h.boot_id = bootId;
  h.rescue = rescueActive();
  h.width = (uint32_t)displayWidth;
  send();
}

void sendStatus(bool timeSynced) {
  const BusRecoveryState& bus = followerBusRecovery();
  wlClear(out);
  out.which_body = wl_ToMaster_status_tag;
  wl_Status& s = out.body.status;
  s.up_s = millis() / 1000;
  s.heap = ESP.getFreeHeap();
  s.min_heap = followerMinHeap();
  s.max_block = ESP.getMaxFreeBlockSize();
  s.rssi = WiFi.RSSI();
  s.tx_power = (uint32_t)(followerTxPowerDbm10() * 4 / 10);
  s.bus_tx = followerBusTxCount();
  s.bus_err = followerBusErrCount();
  s.bus_dead = bus.dead;
  s.bus_episodes = bus.episodes;
  s.escalations = escalationCount(escalationRecordGet());
  s.busy = false;
  s.image_size = ESP.getSketchSize();
  s.time_synced = timeSynced;
  if (send()) lastStatusMs = millis();
}

void handle(const wl_ToRow& m, const FollowerClusterView& view) {
  if (!welcomed) {
    if (m.which_body != wl_ToRow_welcome_tag ||
        !followerLinkMasterAccepted(view.leaderName.c_str(), m.body.welcome.master_id)) {
      drop(F("not the master this row is paired with"));
      return;
    }
    welcomed = true;
    connectCount++;
    backoffMs = 0;
    // A new connection is a new render sequence: the master numbers renders
    // per connection, and the phase machine accepts any new epoch.
    clusterHandleJoin(view.leaderName, view.leaderHost, view.row, bootId + connectCount,
                      String(), String());
    SerialPrintln(F("link: connected to the master"));
    sendStatus(view.sntpSynced);
    return;
  }
  clusterHandlePing();  // any message from the master is contact
  switch (m.which_body) {
    case wl_ToRow_show_tag: {
      const wl_Show& s = m.body.show;
      if (clusterHandleRender(bootId + connectCount, s.render_id, String(s.text), (int)s.speed,
                              s.commit_at_ms) == ClusterRenderVerdict::Apply) {
        wlClear(out);
        out.which_body = wl_ToMaster_shown_tag;
        out.body.shown.render_id = s.render_id;
        send();
      }
      break;
    }
    case wl_ToRow_quiet_tag:
      clusterNoteLeaderQuiet(m.body.quiet.on);
      break;
    case wl_ToRow_ping_tag:
      wlClear(out);
      out.which_body = wl_ToMaster_pong_tag;
      send();
      break;
    case wl_ToRow_restart_tag:
      isPendingReboot = true;
      break;
    case wl_ToRow_release_tag:
      clusterHandleLeave();
      drop(F("released by the master"));
      break;
    default:
      break;  // a message this build does not act on yet
  }
}

}  // namespace

void linkLoopTick() {
  if (bootId == 0) bootId = ESP.random() | 1;
  const FollowerClusterView view = clusterViewGet();
  const bool paired = view.phase != ClusterFollowerPhase::Standalone && view.leaderHost.length() > 0;

  if (!paired || WiFi.status() != WL_CONNECTED) {
    if (sock.connected() || welcomed) drop(F("no master to talk to"));
    return;
  }

  if (!sock.connected()) {
    if (welcomed) drop(F("connection lost"));
    if ((int32_t)(millis() - nextDialMs) < 0) return;
    char host[48];
    if (!followerLinkHostPart(view.leaderHost.c_str(), host, sizeof(host))) return;
    sock.setTimeout(FOLLOWER_LINK_CONNECT_TIMEOUT_MS);
    if (!sock.connect(host, WALL_LINK_PORT)) {
      drop(F("master not reachable"));
      return;
    }
    sock.setNoDelay(true);
    // The core's connected() can stay true on a dead link; TCP keepalive
    // closes it for us (idle 15 s, 3 probes 5 s apart).
    sock.keepAlive(15, 5, 3);
    reader.reset();
    connectedAtMs = millis();
    sendHello();
    return;
  }

  int avail = sock.available();
  while (avail > 0) {
    uint8_t chunk[64];
    const int got = sock.read(chunk, avail < (int)sizeof(chunk) ? avail : (int)sizeof(chunk));
    if (got <= 0) break;
    avail -= got;
    size_t fed = 0;
    while (fed < (size_t)got) {
      fed += reader.feed(chunk + fed, (size_t)got - fed);
      while (reader.peek() == WlFeed::Message) {
        wl_ToRow m = wl_ToRow_init_zero;
        if (reader.decode(wl_ToRow_fields, &m)) handle(m, view);
        reader.pop();
        if (!sock.connected()) return;
      }
      if (reader.peek() == WlFeed::Bad) {
        drop(F("malformed message"));
        return;
      }
    }
  }

  if (!welcomed) {
    if (followerLinkElapsed(millis(), connectedAtMs, FOLLOWER_LINK_WELCOME_TIMEOUT_MS)) {
      drop(F("no Welcome"));
    }
    return;
  }
  if (followerLinkElapsed(millis(), lastStatusMs, FOLLOWER_LINK_STATUS_INTERVAL_MS)) {
    sendStatus(view.sntpSynced);
  }
}

FollowerLinkView linkViewGet() {
  FollowerLinkView v;
  v.connected = welcomed;
  v.connects = connectCount;
  v.drops = dropCount;
  return v;
}
