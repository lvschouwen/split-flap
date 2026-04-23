#include "web_log.h"

// Byte-oriented ring buffer. We capture raw output rather than framed lines
// so callers don't need to agree on where a line ends; readers get the last
// WEBLOG_SIZE bytes of output with newlines intact.

namespace {

char         g_buf[WEBLOG_SIZE];
size_t       g_head = 0;
bool         g_wrapped = false;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

}  // namespace

WebLogPrinter webLogPrinter;

size_t WebLogPrinter::write(uint8_t b) {
  webLogAppend(reinterpret_cast<const char*>(&b), 1);
  return 1;
}

size_t WebLogPrinter::write(const uint8_t* buffer, size_t size) {
  webLogAppend(reinterpret_cast<const char*>(buffer), size);
  return size;
}

void webLogAppend(const char* data, size_t len) {
  if (data == nullptr || len == 0) return;

  portENTER_CRITICAL(&g_mux);
  for (size_t i = 0; i < len; i++) {
    g_buf[g_head++] = data[i];
    if (g_head >= WEBLOG_SIZE) {
      g_head = 0;
      g_wrapped = true;
    }
  }
  portEXIT_CRITICAL(&g_mux);
}

String webLogRead() {
  portENTER_CRITICAL(&g_mux);
  const size_t head    = g_head;
  const bool   wrapped = g_wrapped;
  // Copy into a local snapshot under the lock so the caller can walk it
  // without holding it — keeps the critical section O(WEBLOG_SIZE) once.
  static char snap[WEBLOG_SIZE];
  for (size_t i = 0; i < WEBLOG_SIZE; i++) snap[i] = g_buf[i];
  portEXIT_CRITICAL(&g_mux);

  String out;
  if (wrapped) {
    out.reserve(WEBLOG_SIZE);
    for (size_t i = head; i < WEBLOG_SIZE; i++) out += snap[i];
    for (size_t i = 0; i < head; i++)           out += snap[i];
  } else {
    out.reserve(head);
    for (size_t i = 0; i < head; i++)           out += snap[i];
  }
  return out;
}
