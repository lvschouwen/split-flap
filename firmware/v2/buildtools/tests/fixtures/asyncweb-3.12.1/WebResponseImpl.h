// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright 2016-2026 Hristo Gochkov, Mathieu Carbou, Emil Muratov, Will Miles

// Excerpt of ESP32Async/ESPAsyncWebServer v3.12.1 src/WebResponseImpl.h. Do not edit.
  std::vector<uint8_t> _cache;
  // intermediate buffer to copy outbound data to, also it will keep pending data between _send calls
  std::unique_ptr<std::array<uint8_t, ASYNC_RESPONCE_BUFF_SIZE> > _send_buffer;
  // buffer data size specifiers
  size_t _send_buffer_offset{0}, _send_buffer_len{0};
  size_t _readDataFromCacheOrContent(uint8_t *data, const size_t len);
  size_t _fillBufferAndProcessTemplates(uint8_t *buf, size_t maxLen);

