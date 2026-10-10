// ReleaseTarget.h — the site, the signature check and the hash of
// ReleaseFetch.h on the ESP32-S3 (#583): HTTPS to RELEASE_HOST against the
// framework's certificate bundle, ECDSA P-256 over SHA-256 with the key of
// ReleaseSource.h. Target only; included by the one file of a tree that
// talks to the release site.
//
// The request carries nothing about the board. It follows no redirect: a
// release is fetched from RELEASE_HOST and from nowhere else.
//
// The build needs CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y: with the TLS library's
// memory internal the connection cannot be set up at all (spec, section 10).
#pragma once

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include "ReleaseFetch.h"

// For the connection and for every read of it.
#define RELEASE_HTTP_TIMEOUT_MS 10000

class ReleaseSite : public ReleaseHooks {
 public:
  ReleaseSite() { mbedtls_sha256_init(&sha_); }
  ~ReleaseSite() override {
    close();
    mbedtls_sha256_free(&sha_);
  }
  ReleaseSite(const ReleaseSite&) = delete;
  ReleaseSite& operator=(const ReleaseSite&) = delete;

  int get(const char* url, uint8_t* out, size_t cap) override {
    long length = 0;
    if (!start(url, length)) {
      close();
      return -1;
    }
    if (length > (long)cap) {
      close();
      return -2;
    }
    size_t got = 0;
    int result = -1;
    for (;;) {
      if (got == cap) {
        // Full: anything more is too much, whatever length was announced.
        char more;
        const int n = esp_http_client_read(client_, &more, 1);
        result = n == 0 ? (int)got : n > 0 ? -2 : -1;
        break;
      }
      const int n = esp_http_client_read(client_, (char*)out + got, (int)(cap - got));
      if (n < 0) break;
      if (n == 0) {
        result = (int)got;
        break;
      }
      got += (size_t)n;
    }
    close();
    return result;
  }

  bool signatureOk(const uint8_t* message, size_t len, const char* signature,
                   size_t signatureLen) override {
    while (signatureLen > 0 && (signature[signatureLen - 1] == '\n' ||
                                signature[signatureLen - 1] == '\r' ||
                                signature[signatureLen - 1] == ' ')) {
      signatureLen--;
    }
    uint8_t der[80];  // a P-256 signature in DER is 72 bytes at most
    size_t derLen = 0;
    if (mbedtls_base64_decode(der, sizeof(der), &derLen, (const unsigned char*)signature,
                              signatureLen) != 0) {
      return false;
    }
    uint8_t digest[32];
    if (mbedtls_sha256(message, len, digest, 0) != 0) return false;
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    bool ok = mbedtls_pk_parse_public_key(&key, RELEASE_PUBLIC_KEY_DER,
                                          RELEASE_PUBLIC_KEY_DER_LEN) == 0 &&
              mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest, sizeof(digest), der, derLen) == 0;
    mbedtls_pk_free(&key);
    return ok;
  }

  long open(const char* url) override {
    long length = 0;
    if (!start(url, length) || length <= 0) return -1;
    return length;
  }

  int read(uint8_t* out, size_t cap) override {
    if (client_ == nullptr) return -1;
    return esp_http_client_read(client_, (char*)out, (int)cap);
  }

  void close() override {
    if (client_ == nullptr) return;
    esp_http_client_close(client_);
    esp_http_client_cleanup(client_);
    client_ = nullptr;
  }

  void hashStart() override { mbedtls_sha256_starts(&sha_, 0); }
  void hashAdd(const uint8_t* data, size_t len) override { mbedtls_sha256_update(&sha_, data, len); }
  void hashEnd(uint8_t out[32]) override { mbedtls_sha256_finish(&sha_, out); }

 private:
  esp_http_client_handle_t client_ = nullptr;
  mbedtls_sha256_context sha_;

  // Connected and answered 200. `length` is what the answer announces, 0
  // when it announces none.
  bool start(const char* url, long& length) {
    close();
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = RELEASE_HTTP_TIMEOUT_MS;
    config.disable_auto_redirect = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 4096;
    client_ = esp_http_client_init(&config);
    if (client_ == nullptr) return false;
    if (esp_http_client_open(client_, 0) != ESP_OK) return false;
    length = (long)esp_http_client_fetch_headers(client_);
    return length >= 0 && esp_http_client_get_status_code(client_) == 200;
  }
};
