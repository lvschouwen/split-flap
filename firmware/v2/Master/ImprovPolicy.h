#pragma once
// ImprovPolicy.h — WiFi credentials over USB, by the Improv serial protocol
// (#601; https://www.improv-wifi.com/serial/). The web flasher's installer
// speaks it right after a flash: it asks what the board is, lists the
// networks the board sees, and hands over a name and a password.
//
// Pure: bytes in, packets and intents out. The packet parser and the reply
// payloads are the Improv SDK's (improv.h); this header frames them for the
// serial port and decides what each request gets. ImprovService.cpp moves the
// bytes; joining stays with WifiPolicy.h / WifiService.cpp, which only ever
// sees a staged name and password, exactly as from the setup page.
//
// Credentials are saved by a restart (WifiAction::SaveAndReboot), so the
// request that brings them is answered in two parts: "provisioning" before
// the restart, and the result — the board's address — from the boot that
// joined. What links the two is ImprovPendingRecord, kept in memory a
// software reset leaves alone. The installer waits 45 s for that result.
//
// A packet is only recognised at the start of a line, so every packet is
// written with a newline before and after it: log text shares the port.
// Natively tested (test/test_improv_policy).

#include <Arduino.h>
#include <improv.h>

#include <string>
#include <vector>

#include "SettingsLimits.h"
#include "SettingsValidation.h"

// "IMPROV", version, type, length, up to 255 bytes, checksum.
#define IMPROV_FRAME_MAX 265

// What the radio is doing, as the installer needs to know it.
enum class ImprovLink : uint8_t {
  Ready,    // no network yet and none being joined: credentials are welcome
  Joining,  // a join is under way
  Online,
};

struct ImprovFacts {
  ImprovLink link = ImprovLink::Ready;
  // A scan now would not disturb a join (the setup network is up, or the
  // board is online).
  bool canScan = false;
  std::string url;   // the board's page; "" unless Online
  std::string name;  // the board's own name
  std::string rev;
};

typedef std::vector<uint8_t> ImprovPacket;

struct ImprovOut {
  std::vector<ImprovPacket> packets;
  // Start a scan; its networks are answered later by improvScanReply().
  bool scan = false;
  // A valid name and password arrived: stage them and note which port asked.
  bool credentials = false;
  std::string ssid;
  std::string password;
};

struct ImprovNetwork {
  std::string ssid;
  int rssi = -127;
  bool secure = true;
};

// One port's receive side: the frame being collected and the error the
// installer was last told of.
struct ImprovRx {
  uint8_t frame[IMPROV_FRAME_MAX + 1];
  size_t length = 0;
  improv::Error told = improv::ERROR_NONE;
};

inline ImprovPacket improvPacket(improv::ImprovSerialType type,
                                 const std::vector<uint8_t>& data) {
  ImprovPacket out = {'\n', 'I', 'M', 'P', 'R', 'O', 'V',
                      improv::IMPROV_SERIAL_VERSION, (uint8_t)type,
                      (uint8_t)data.size()};
  out.insert(out.end(), data.begin(), data.end());
  uint8_t checksum = 0;
  for (size_t i = 1; i < out.size(); i++) checksum += out[i];
  out.push_back(checksum);
  out.push_back('\n');
  return out;
}

inline ImprovPacket improvStatePacket(improv::State state) {
  return improvPacket(improv::TYPE_CURRENT_STATE, {(uint8_t)state});
}

inline ImprovPacket improvErrorPacket(improv::Error error) {
  return improvPacket(improv::TYPE_ERROR_STATE, {(uint8_t)error});
}

inline ImprovPacket improvResultPacket(improv::Command command,
                                       const std::vector<std::string>& strings) {
  return improvPacket(improv::TYPE_RPC_RESPONSE,
                      improv::build_rpc_response(command, strings, false));
}

inline improv::State improvStateOf(ImprovLink link) {
  switch (link) {
    case ImprovLink::Online:
      return improv::STATE_PROVISIONED;
    case ImprovLink::Joining:
      return improv::STATE_PROVISIONING;
    default:
      return improv::STATE_AUTHORIZED;
  }
}

inline void improvTellError(ImprovRx& rx, improv::Error error, ImprovOut& out) {
  rx.told = error;
  out.packets.push_back(improvErrorPacket(error));
}

// One request, already parsed and checksummed by the SDK.
inline void improvHandle(ImprovRx& rx, const improv::ImprovCommand& command,
                         const ImprovFacts& facts, ImprovOut& out) {
  // A request clears the error of the one before it.
  if (rx.told != improv::ERROR_NONE) improvTellError(rx, improv::ERROR_NONE, out);

  switch (command.command) {
    case improv::WIFI_SETTINGS: {
      String ssid(command.ssid.c_str());
      String password(command.password.c_str());
      if (!isValidWifiSsidValue(ssid, LEN_WIFI_SSID) ||
          !isValidWifiPasswordValue(password, LEN_WIFI_PASSWORD)) {
        improvTellError(rx, improv::ERROR_UNABLE_TO_CONNECT, out);
        return;
      }
      out.credentials = true;
      out.ssid = command.ssid;
      out.password = command.password;
      out.packets.push_back(improvStatePacket(improv::STATE_PROVISIONING));
      return;
    }
    case improv::GET_CURRENT_STATE:
      out.packets.push_back(improvStatePacket(improvStateOf(facts.link)));
      if (facts.link == ImprovLink::Online) {
        out.packets.push_back(
            improvResultPacket(improv::GET_CURRENT_STATE, {facts.url}));
      }
      return;
    case improv::GET_DEVICE_INFO:
      out.packets.push_back(improvResultPacket(
          improv::GET_DEVICE_INFO,
          {"Split-Flap", facts.rev, "ESP32-S3", facts.name}));
      return;
    case improv::GET_WIFI_NETWORKS:
      if (facts.canScan) {
        out.scan = true;
      } else {
        // The list ends with an empty result; alone it means "none seen",
        // and the installer then asks for the name by hand.
        out.packets.push_back(improvResultPacket(improv::GET_WIFI_NETWORKS, {}));
      }
      return;
    case improv::UNKNOWN:
    case improv::BAD_CHECKSUM:
      improvTellError(rx, improv::ERROR_INVALID_RPC, out);
      return;
    default:
      improvTellError(rx, improv::ERROR_UNKNOWN_RPC, out);
      return;
  }
}

// One received byte. Anything that is not an Improv frame — a terminal's
// keystrokes, line noise — falls out of the parser at its first wrong byte.
inline void improvFeed(ImprovRx& rx, uint8_t byte, const ImprovFacts& facts,
                       ImprovOut& out) {
  if (rx.length > IMPROV_FRAME_MAX) rx.length = 0;
  size_t at = rx.length;
  rx.frame[rx.length++] = byte;
  bool more = improv::parse_improv_serial_byte(
      at, byte, rx.frame,
      [&](improv::ImprovCommand command) -> bool {
        improvHandle(rx, command, facts, out);
        return false;  // frame done
      },
      [&](improv::Error error) { improvTellError(rx, error, out); });
  if (!more) {
    rx.length = 0;
    // The byte that broke a frame may open the next one.
    if (at > 0 && byte == 'I') rx.frame[rx.length++] = byte;
  }
}

// The answer to a scan: one result per network, then the empty one.
inline void improvScanReply(const std::vector<ImprovNetwork>& networks,
                            ImprovOut& out) {
  for (const ImprovNetwork& network : networks) {
    out.packets.push_back(improvResultPacket(
        improv::GET_WIFI_NETWORKS,
        {network.ssid, std::to_string(network.rssi),
         network.secure ? "YES" : "NO"}));
  }
  out.packets.push_back(improvResultPacket(improv::GET_WIFI_NETWORKS, {}));
}

// --- the answer that crosses the restart -------------------------------------

#define IMPROV_PENDING_MAGIC 0x56504D49UL  // "IMPV" LE
#define IMPROV_PORTS 2

// Which port is owed the result of its credentials. RAM that a software reset
// keeps holds garbage after power-on, hence the magic and the complement.
struct ImprovPendingRecord {
  uint32_t magic;
  uint8_t port;
  uint8_t check;  // ~port
};

inline void improvPendingEncode(ImprovPendingRecord& r, uint8_t port) {
  r.magic = IMPROV_PENDING_MAGIC;
  r.port = port;
  r.check = (uint8_t)~port;
}

inline void improvPendingClear(ImprovPendingRecord& r) { r.magic = 0; }

// The port owed an answer, or -1.
inline int improvPendingDecode(const ImprovPendingRecord& r) {
  if (r.magic != IMPROV_PENDING_MAGIC) return -1;
  if (r.check != (uint8_t)~r.port) return -1;
  if (r.port >= IMPROV_PORTS) return -1;
  return r.port;
}

// What the port that brought the credentials is told once their fate is
// known. Returns true when it is: online, or back in the setup network
// (`setupOpen`) after every join attempt failed. Until then, nothing.
inline bool improvOutcome(const ImprovFacts& facts, bool setupOpen,
                          ImprovOut& out) {
  if (facts.link == ImprovLink::Online) {
    out.packets.push_back(improvStatePacket(improv::STATE_PROVISIONED));
    out.packets.push_back(
        improvResultPacket(improv::WIFI_SETTINGS, {facts.url}));
    return true;
  }
  if (setupOpen) {
    out.packets.push_back(improvErrorPacket(improv::ERROR_UNABLE_TO_CONNECT));
    out.packets.push_back(improvStatePacket(improv::STATE_AUTHORIZED));
    return true;
  }
  return false;
}
