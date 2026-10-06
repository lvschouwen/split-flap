#pragma once
// NetLivenessPolicy.h — end-to-end network liveness (#501). The WiFi policy
// watches the link bit; this watches whether traffic actually moves while the
// link says it is up. A board whose IP stack or web server has wedged keeps
// its association, keeps feeding the task watchdog, and stays off the network
// until someone pulls the plug.
//
// Two probes through the real stack (NetLiveness.cpp):
//   gateway  — a TCP connect: a round trip over the radio (refused counts)
//   self     — an HTTP request to this board's own web server, answered
//
// Rules, all here and natively tested (test_net_liveness):
//   - A probe can only condemn what it has seen working on this association.
//     A gateway that never answers, or a network that is down from boot, never
//     causes a reboot — which is also what stops a reboot loop on a dead
//     access point. A link drop wipes what was seen: a router that comes back
//     slowly has to prove itself again first.
//   - A link that is down is the WiFi policy's business, not this one's.
//   - No fresh probe result is no evidence: the count starts over.
//   - The patience doubles with every liveness reboot in a row and is given
//     back after a healthy stretch.
#include <stdint.h>

#define NET_LIVENESS_PROBE_INTERVAL_MS 30000UL
// A result older than this says nothing about now (the prober shares its task
// with the pairing requests and can be busy for a while).
#define NET_LIVENESS_PROBE_STALE_MS 120000UL
#define NET_LIVENESS_BASE_MS (10UL * 60UL * 1000UL)
#define NET_LIVENESS_MAX_MS (6UL * 60UL * 60UL * 1000UL)
#define NET_LIVENESS_HEALTHY_RESET_MS (30UL * 60UL * 1000UL)

enum class NetProbe : uint8_t { Unknown = 0, Ok, Fail };

enum class NetLivenessCause : uint8_t { None = 0, Gateway, SelfServer };

struct NetLivenessWatch {
  bool seenOk = false;
  bool bad = false;
  uint32_t badSinceMs = 0;
};

struct NetLivenessState {
  NetLivenessWatch gateway;
  NetLivenessWatch self;
  bool healthy = false;
  uint32_t healthySinceMs = 0;
};

struct NetLivenessVerdict {
  NetLivenessCause reboot = NetLivenessCause::None;
  bool clearStrikes = false;  // a healthy stretch earned the patience back
};

// How long a probe may fail before the board restarts itself, given how many
// liveness reboots in a row came before this boot.
inline uint32_t netLivenessThresholdMs(uint8_t strikes) {
  uint32_t t = NET_LIVENESS_BASE_MS;
  for (uint8_t i = 0; i < strikes && t < NET_LIVENESS_MAX_MS; i++) t *= 2;
  return t < NET_LIVENESS_MAX_MS ? t : NET_LIVENESS_MAX_MS;
}

inline void netLivenessFold(NetLivenessWatch& w, NetProbe result,
                            uint32_t nowMs) {
  if (result == NetProbe::Ok) {
    w.seenOk = true;
    w.bad = false;
  } else if (result == NetProbe::Fail && w.seenOk) {
    if (!w.bad) {
      w.bad = true;
      w.badSinceMs = nowMs;
    }
  } else {
    w.bad = false;  // unknown, or a probe that has never passed
  }
}

// One step, about once a second. `linkUp` = associated with an address.
inline NetLivenessVerdict netLivenessStep(NetLivenessState& st, bool linkUp,
                                          NetProbe gateway, NetProbe self,
                                          uint32_t nowMs, uint8_t strikes) {
  NetLivenessVerdict v;
  if (!linkUp) {
    st.gateway = NetLivenessWatch{};
    st.self = NetLivenessWatch{};
    st.healthy = false;
    return v;
  }
  netLivenessFold(st.gateway, gateway, nowMs);
  netLivenessFold(st.self, self, nowMs);

  const uint32_t limit = netLivenessThresholdMs(strikes);
  if (st.gateway.bad && nowMs - st.gateway.badSinceMs >= limit) {
    v.reboot = NetLivenessCause::Gateway;
  } else if (st.self.bad && nowMs - st.self.badSinceMs >= limit) {
    v.reboot = NetLivenessCause::SelfServer;
  }

  // Healthy = nothing failing that once worked, and something answering. A
  // gateway that never answers on the probed port must not keep the strikes
  // from ever being given back.
  if (!st.gateway.bad && !st.self.bad &&
      (gateway == NetProbe::Ok || self == NetProbe::Ok)) {
    if (!st.healthy) {
      st.healthy = true;
      st.healthySinceMs = nowMs;
    }
    v.clearStrikes = nowMs - st.healthySinceMs >= NET_LIVENESS_HEALTHY_RESET_MS;
  } else {
    st.healthy = false;
  }
  return v;
}

// A result too old to speak for now reads as Unknown.
inline NetProbe netLivenessFresh(NetProbe result, uint32_t resultAtMs,
                                 uint32_t nowMs) {
  // Signed: the prober runs on the other core and may stamp a result a few
  // milliseconds after the caller took `nowMs`.
  return (int32_t)(nowMs - resultAtMs) <= (int32_t)NET_LIVENESS_PROBE_STALE_MS
             ? result
             : NetProbe::Unknown;
}

inline const char* netProbeName(NetProbe p) {
  return p == NetProbe::Ok ? "ok" : p == NetProbe::Fail ? "fail" : "unknown";
}

inline const char* netLivenessCauseText(NetLivenessCause c) {
  return c == NetLivenessCause::Gateway
             ? "gateway unreachable while the WiFi link is up"
             : "own web server not answering";
}
