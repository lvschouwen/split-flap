#pragma once
// FollowerPairPolicy.h — who may become this row's master (POST /pair), pure
// and natively tested by test_follower_pair. A row obeys one master, known by
// its id and the address it paired from. The id is no secret (this row serves
// it, the master advertises it), so the address is part of the rule:
//  - no master yet: the caller becomes it;
//  - its own master, same address: nothing changes;
//  - its own master's id from another address: taken only once the master is
//    out of contact (CLUSTER_CONTACT_FRESH_MS without a message) — a master
//    that moved, never a caller redirecting a working row;
//  - another id: taken only once its own master is written off (LeaderLost,
//    CLUSTER_GRACE_MS) — a replaced master needs no step on the row.

#include <stdint.h>
#include <string.h>

// wl.Welcome.master_id holds this many characters.
#define FOLLOWER_MASTER_ID_MAX 32

enum class PairVerdict : uint8_t {
  Store,   // becomes this row's master, or its master's new address
  Same,    // already is, at this address: nothing to do
  Refuse,  // this row's master is still there
  BadId,
};

// What the row holds when a caller asks.
struct FollowerPairing {
  bool paired = false;
  const char* masterId = "";
  const char* masterHost = "";
  bool masterInContact = false;  // a message from it within the contact window
  bool masterLost = false;       // written off: the row shows its fallback
};

// An id is stored, compared with Welcome and served back: printable ASCII
// without spaces, 1..FOLLOWER_MASTER_ID_MAX characters.
inline bool followerPairIdValid(const char* id) {
  if (id == nullptr) return false;
  const size_t n = strlen(id);
  if (n == 0 || n > FOLLOWER_MASTER_ID_MAX) return false;
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)id[i];
    if (c < 0x21 || c > 0x7E) return false;
  }
  return true;
}

inline PairVerdict followerPairDecide(const char* id, const char* callerHost,
                                      const FollowerPairing& now) {
  if (!followerPairIdValid(id)) return PairVerdict::BadId;
  if (!now.paired) return PairVerdict::Store;
  if (strcmp(id, now.masterId) != 0) {
    return now.masterLost ? PairVerdict::Store : PairVerdict::Refuse;
  }
  if (strcmp(callerHost, now.masterHost) == 0) return PairVerdict::Same;
  return now.masterInContact ? PairVerdict::Refuse : PairVerdict::Store;
}
