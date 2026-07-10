#pragma once
// OtaService — native S3 A/B rollback integration for the OTA slice (#190).
//
// The bootloader does the heavy lifting (v1's RTC-cookie machinery is NOT
// ported — decided 2026-07-09): a freshly flashed image boots PENDING_VERIFY
// and is reverted on the next reset unless confirmed. This TU defers the
// Arduino core's auto-confirm (strong verifyRollbackLater() -> true) and
// confirms only once a netif is up — boot + radio + web stack alive is the
// health bar. The /settings verdict fields are synthesized from esp_ota
// partition state via the pure OtaStatus.h (natively tested).

#include <Arduino.h>

#include "OtaStatus.h"

// setup() context, before tasksInit(): snapshots this boot's partition
// state (pending-verify? did a rollback happen?) and creates the guard
// mutex for the cross-task reads below.
void otaServiceInit();

// netTask context: confirms a pending image (first call wins; later calls
// no-op). WifiService calls this from both startOnline() and startPortal()
// — a portal boot is a healthy boot, and the portal-timeout reboot must not
// cost us an unconfirmed image.
void otaHealthConfirm();

// Async-handler safe: mutex-guarded snapshot of the synthesized verdict for
// GET /settings and GET /debug/ota.
OtaVerdict otaVerdictSnapshot();

// Async-handler safe: small JSON blob for GET /debug/ota — partition labels,
// this boot's ota state, and the verdict fields. Bench diagnostics only.
String otaDebugJson();
