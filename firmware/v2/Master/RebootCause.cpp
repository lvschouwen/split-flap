#include "RebootCause.h"

#include <Preferences.h>

// Own tiny namespace, not the "splitflap" settings namespace: the settings
// store is a seam with native-test policy behind it (#185); this breadcrumb
// is target-only glue with exactly one key and a write-then-consume
// lifecycle that never interacts with settings.
static const char* kNamespace = "sfboot";
static const char* kCauseKey = "cause";

void rebootCauseStamp(const String& cause) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/false)) return;
  prefs.putString(kCauseKey, cause);
  prefs.end();
}

String rebootCauseConsume() {
  // Cached: the FIRST call (main.cpp setup(), before any init that can
  // panic) reads and clears NVS; every later call returns the cache. Without
  // the early clear, a stamp could outlive a boot that panics mid-init and
  // be blamed on whichever later boot first reaches the consume point.
  static bool consumed = false;
  static String cached;
  if (consumed) return cached;
  consumed = true;
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/false)) return cached;
  cached = prefs.getString(kCauseKey, "");
  if (cached.length() > 0) prefs.remove(kCauseKey);
  prefs.end();
  return cached;
}
