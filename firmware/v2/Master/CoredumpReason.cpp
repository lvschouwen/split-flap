#include "CoredumpReason.h"

#include <esp_app_desc.h>
#include <esp_core_dump.h>
#include <stdlib.h>
#include <string.h>

static bool dumpIsThisBuild() {
  auto* summary = (esp_core_dump_summary_t*)calloc(1, sizeof(esp_core_dump_summary_t));
  if (summary == nullptr) return false;
  bool ours = false;
  if (esp_core_dump_get_summary(summary) == ESP_OK) {
    char running[APP_ELF_SHA256_SZ];
    esp_app_get_elf_sha256(running, sizeof(running));
    char dump[APP_ELF_SHA256_SZ];
    memcpy(dump, summary->app_elf_sha256, sizeof(dump));
    dump[sizeof(dump) - 1] = '\0';
    ours = dump[0] != '\0' && strncmp(dump, running, strlen(dump)) == 0;
  }
  free(summary);
  return ours;
}

bool coredumpPanicReason(char* out, size_t cap) {
  if (cap == 0) return false;
  out[0] = '\0';
  if (esp_core_dump_image_check() != ESP_OK || !dumpIsThisBuild()) return false;
  if (esp_core_dump_get_panic_reason(out, cap) != ESP_OK) {
    out[0] = '\0';
    return false;
  }
  for (char* p = out; *p; p++)
    if (*p == '\n' || *p == '\r') *p = ' ';
  return out[0] != '\0';
}
