#pragma once
// OtaService — native S3 A/B rollback integration for the OTA slice (#190).
//
// The bootloader does the heavy lifting (v1's RTC-cookie machinery is NOT
// ported — decided 2026-07-09): a freshly flashed image boots PENDING_VERIFY
// and is reverted on the next reset unless confirmed. This TU defers the
// Arduino core's auto-confirm (strong verifyRollbackLater() -> true) and
// confirms once the full single-threaded setup() has proven the image doesn't
// hard-crash — before tasksInit() starts the display/WiFi inrush (#305). The
// old bar was first-netif-up, but that sits on the far side of the inrush,
// whose current draw can trip the S3 brownout detector on a verify-boot and
// roll a good image back over a sag it survives at steady state. The /settings
// verdict fields are synthesized from esp_ota partition state via the pure
// OtaStatus.h (natively tested).

#include <Arduino.h>

#include "OtaStatus.h"

// setup() context, before tasksInit(): snapshots this boot's partition
// state (pending-verify? did a rollback happen?) and creates the guard
// mutex for the cross-task reads below.
void otaServiceInit();

// Confirms a pending image (first successful call wins; later calls no-op).
// Primary caller is setup() pre-inrush (#305); WifiService still calls it on
// netif-up from both startOnline() and startPortal() as a fallback (a portal
// boot is a healthy boot). Safe from either context — the callers are
// temporally separated and the shared state is mutex-guarded.
void otaHealthConfirm();

// Async-handler safe: mutex-guarded snapshot of the synthesized verdict for
// GET /settings and GET /debug/ota.
OtaVerdict otaVerdictSnapshot();

// Async-handler safe: small JSON blob for GET /debug/ota — partition labels,
// this boot's ota state, and the verdict fields. Bench diagnostics only.
String otaDebugJson();
