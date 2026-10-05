#include "NetLiveness.h"

#include <Update.h>
#include <WiFi.h>
#include <lwip/sockets.h>

#include <atomic>

namespace {

constexpr uint32_t PROBE_CONNECT_TIMEOUT_MS = 1500;
constexpr uint32_t STRIKES_MAGIC = 0x4E4C5631UL;  // "NLV1"

// Written by clusterTask, read by netTask and the web layer.
std::atomic<uint8_t> gatewayResult{(uint8_t)NetProbe::Unknown};
std::atomic<uint8_t> selfResult{(uint8_t)NetProbe::Unknown};
std::atomic<uint32_t> resultAtMs{0};
std::atomic<bool> everProbed{false};

struct StrikeRecord {
  uint32_t magic;
  uint32_t strikes;
};
RTC_NOINIT_ATTR StrikeRecord strikeRecord;

// Connects through the real stack and, with `request` set, sends it and
// waits for the start of an HTTP reply.
//
// Gateway (no request): refused counts as reachable — the SYN went out and an
// answer came back, which is the round trip in question.
// Own server (request): only an answered request counts. A connect alone is
// completed by the IP stack before the web server's task has done anything,
// and a refusal means the listener is gone.
//
// Unknown when the shortage is this board's own (no socket, no memory): that
// is not evidence about the network.
NetProbe tcpProbe(IPAddress ip, uint16_t port, const char* request) {
  int s = lwip_socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return NetProbe::Unknown;
  int flags = lwip_fcntl(s, F_GETFL, 0);
  if (flags < 0 || lwip_fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) {
    lwip_close(s);  // a blocking connect could outlast the task watchdog
    return NetProbe::Unknown;
  }
  // Close with a reset: a probe every 30 s must not park its connection
  // blocks in TIME_WAIT, out of a pool the web server and MQTT share.
  struct linger hard = {};
  hard.l_onoff = 1;
  hard.l_linger = 0;
  lwip_setsockopt(s, SOL_SOCKET, SO_LINGER, &hard, sizeof(hard));

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = (uint32_t)ip;
  struct timeval tv = {};
  tv.tv_sec = PROBE_CONNECT_TIMEOUT_MS / 1000;
  tv.tv_usec = (PROBE_CONNECT_TIMEOUT_MS % 1000) * 1000;

  int err = 0;
  bool connected = false;
  if (lwip_connect(s, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
    connected = true;
  } else if (errno == EINPROGRESS) {
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(s, &writable);
    if (lwip_select(s + 1, nullptr, &writable, nullptr, &tv) > 0) {
      socklen_t len = sizeof(err);
      lwip_getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
      connected = err == 0;
    } else {
      err = ETIMEDOUT;
    }
  } else {
    err = errno;
  }

  NetProbe result = NetProbe::Fail;
  if (err == ENOMEM || err == ENOBUFS) {
    result = NetProbe::Unknown;
  } else if (request == nullptr) {
    result = (connected || err == ECONNREFUSED) ? NetProbe::Ok : NetProbe::Fail;
  } else if (connected) {
    size_t len = strlen(request);
    if (lwip_send(s, request, len, 0) == (int)len) {
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(s, &readable);
      char head[8] = {};
      if (lwip_select(s + 1, &readable, nullptr, nullptr, &tv) > 0 &&
          lwip_recv(s, head, 5, 0) == 5 && memcmp(head, "HTTP/", 5) == 0) {
        result = NetProbe::Ok;
      }
    }
  }
  lwip_close(s);
  return result;
}

}  // namespace

void netLivenessProbeTick() {
  static uint32_t nextProbeMs = 0;
  uint32_t now = millis();
  if ((int32_t)(now - nextProbeMs) < 0) return;
  nextProbeMs = now + NET_LIVENESS_PROBE_INTERVAL_MS;
  // No link: nothing to probe, and the results on file go stale by
  // themselves. An OTA upload owns the stack's attention and ends in a
  // restart anyway.
  if (WiFi.status() != WL_CONNECTED || Update.isRunning()) return;
  IPAddress self = WiFi.localIP();
  IPAddress gateway = WiFi.gatewayIP();
  if ((uint32_t)self == 0 || (uint32_t)gateway == 0) return;
  NetProbe gw = tcpProbe(gateway, 80, nullptr);
  // A path nothing serves: the 404 comes from the web server's own task, so
  // it proves that task runs — at the cost of the smallest reply there is.
  NetProbe own = tcpProbe(self, 80,
                          "GET /net-liveness HTTP/1.0\r\n"
                          "Connection: close\r\n\r\n");
  gatewayResult.store((uint8_t)gw);
  selfResult.store((uint8_t)own);
  resultAtMs.store(millis());
  everProbed.store(true);
}

NetProbe netLivenessGateway(uint32_t nowMs) {
  if (!everProbed.load()) return NetProbe::Unknown;
  return netLivenessFresh((NetProbe)gatewayResult.load(), resultAtMs.load(),
                          nowMs);
}

NetProbe netLivenessSelf(uint32_t nowMs) {
  if (!everProbed.load()) return NetProbe::Unknown;
  return netLivenessFresh((NetProbe)selfResult.load(), resultAtMs.load(),
                          nowMs);
}

uint8_t netLivenessStrikes() {
  if (strikeRecord.magic != STRIKES_MAGIC) return 0;
  return strikeRecord.strikes > 255 ? 255 : (uint8_t)strikeRecord.strikes;
}

void netLivenessAddStrike() {
  uint32_t n = netLivenessStrikes();
  strikeRecord.magic = STRIKES_MAGIC;
  strikeRecord.strikes = n < 255 ? n + 1 : 255;
}

void netLivenessClearStrikes() {
  strikeRecord.magic = STRIKES_MAGIC;
  strikeRecord.strikes = 0;
}

String netLivenessJson() {
  uint32_t now = millis();
  String out = "{\"gw\":\"";
  out += netProbeName(netLivenessGateway(now));
  out += "\",\"self\":\"";
  out += netProbeName(netLivenessSelf(now));
  out += "\",\"strikes\":";
  out += (unsigned)netLivenessStrikes();
  out += ",\"limitMin\":";
  out += (unsigned long)(netLivenessThresholdMs(netLivenessStrikes()) / 60000UL);
  out += '}';
  return out;
}
