// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright 2016-2026 Hristo Gochkov, Mathieu Carbou, Emil Muratov, Will Miles

// Excerpt (write_send_buffs) of ESP32Async/ESPAsyncWebServer v3.12.1 src/WebResponses.cpp,
// the function patch_asyncweb.py patches for #631. Do not edit: it is the pinned source.

size_t AsyncAbstractResponse::write_send_buffs(AsyncWebServerRequest *request, size_t len, uint32_t time) {
  (void)time;
  if (!_sourceValid()) {
    _state = RESPONSE_FAILED;
    request->client()->close();
    return 0;
  }

#if ASYNCWEBSERVER_USE_CHUNK_INFLIGHT
  /*
    for response payloads with unknown length or length larger than TCP_WND we need to control AsyncTCP's queue and in-flight fragmentation.
    Either user callback could fill buffer with very small chunks or long running large response could receive a lot of poll() calls here,
    both could flood asynctcp's queue with large number of events to handle and fragment socket buffer space for large responses.
    Let's ignore polled acks and acks in case when available window size is less than our used buffer size since we won't be able to fill and send it whole
    That way we could balance on having at least half tcp win in-flight while minimizing send/ack events in asynctcp Q
    This could decrease sustained bandwidth for one single connection but would drastically improve parallelism and equalize bandwidth sharing
  */
  // return a credit for each chunk of acked data (polls does not give any credits)
  if (len) {
    ++_in_flight_credit;
    _in_flight -= std::min(len, _in_flight);
  }

  if (_chunked || !_sendContentLength || (_sentLength > CONFIG_LWIP_TCP_WND_DEFAULT)) {
    if (!_in_flight_credit || (ASYNC_RESPONCE_BUFF_SIZE > request->client()->space())) {
      // async_ws_log_d("defer user call in_flight:%u, tcpwin:%u", _in_flight, request->client()->space());
      // take the credit back since we are ignoring this ack and rely on other inflight data acks
      if (len) {
        --_in_flight_credit;
      }
      return 0;
    }
  }
#endif

  // this is not functionally needed in AsyncAbstractResponse itself, but kept for compatibility if some of the derived classes are rely on it somehow
  _ackedLength += len;

  size_t payloadlen{0};  // amount of data to be written to tcp sockbuff during this call, used as return value of this method

  // send http headers first
  if (_state == RESPONSE_HEADERS) {
    // copy headers buffer to sock buffer
    size_t const pcb_written = request->client()->add(_assembled_headers.c_str() + _writtenHeadersLength, _assembled_headers.length() - _writtenHeadersLength);
    _writtenLength += pcb_written;
    _writtenHeadersLength += pcb_written;
    if (_writtenHeadersLength < _assembled_headers.length()) {
// we were not able to fit all headers in current buff, send this part here and return later for the rest
#if ASYNCWEBSERVER_USE_CHUNK_INFLIGHT
      _in_flight += pcb_written;
      --_in_flight_credit;  // take a credit
#endif
      if (!request->client()->send()) {
        // something is wrong, what should we do here?
        request->client()->close();
        return 0;
      }
      return pcb_written;
    }
    // otherwise we've added all the (remainder) headers in current buff
    _state = RESPONSE_CONTENT;
    payloadlen += pcb_written;
    _assembled_headers = String();  // clear
  }

  // send content body
  if (_state == RESPONSE_CONTENT) {
    do {
      if (_send_buffer_len && _send_buffer) {
        // data is pending in buffer from a previous call or previous iteration
        size_t const added_len =
          request->client()->add(reinterpret_cast<char *>(_send_buffer->data() + _send_buffer_offset), _send_buffer_len - _send_buffer_offset);
        if (added_len != _send_buffer_len - _send_buffer_offset) {
          // we were not able to add entire buffer's content to tcp buffs, leave it for later
          // (this should not happen normally unless connection's TCP window suddenly changed from remote or mem pressure)
          _send_buffer_offset += added_len;
          break;
        } else {
          _send_buffer_len = _send_buffer_offset = 0;  // consider buffer empty
        }
        payloadlen += added_len;
      }

      auto tcp_win = request->client()->space();
      if (tcp_win == 0 || _state == RESPONSE_END) {
        break;  // no room left or no more data
      }

      if ((_chunked || !_sendContentLength) && (tcp_win < CONFIG_LWIP_TCP_MSS / 2)) {
        // available window size is not enough to send a new chunk sized half of tcp mss, let's wait for better chance and reduce pressure to AsyncTCP's event Q
        break;
      }

      if (!_send_buffer) {
        auto p = new (std::nothrow) std::array<uint8_t, ASYNC_RESPONCE_BUFF_SIZE>;
        if (p) {
          _send_buffer.reset(p);
          _send_buffer_len = _send_buffer_offset = 0;
        } else {
          break;  // OOM
        }
      }

      if (_chunked) {
        // HTTP 1.1 allows leading zeros in chunk length. Or spaces may be added.
        // See https://datatracker.ietf.org/doc/html/rfc9112#section-7.1
        size_t const readLen =
          _fillBufferAndProcessTemplates(_send_buffer->data() + 6, std::min(_send_buffer->size(), tcp_win) - 8);  // reserve 8 bytes for chunk size data
        if (readLen != RESPONSE_TRY_AGAIN) {
          // Write 4 hex digits directly without null terminator
          static constexpr char hexChars[] = "0123456789abcdef";
          _send_buffer->data()[0] = hexChars[(readLen >> 12) & 0xF];
          _send_buffer->data()[1] = hexChars[(readLen >> 8) & 0xF];
          _send_buffer->data()[2] = hexChars[(readLen >> 4) & 0xF];
          _send_buffer->data()[3] = hexChars[readLen & 0xF];
          _send_buffer->data()[4] = '\r';
          _send_buffer->data()[5] = '\n';
          // data (readLen bytes) is already there
          _send_buffer->at(readLen + 6) = '\r';
          _send_buffer->at(readLen + 7) = '\n';
          _send_buffer_len += readLen + 8;  // set buffers's size to match added data
          _sentLength += readLen;           // data is not sent yet, but we won't get a chance to count this later properly for chunked data
          if (!readLen) {
            // last chunk?
            _state = RESPONSE_END;
          }
        }
      } else {
        // Non-chunked data. We can either have a response:
        // - with a known content-length (example: Json response), in that case we pass the remaining length if lower than tcp_win
        // - or with unknown content-length (see LargeResponse example, like ESP32Cam with streaming), in that case we just fill as much as tcp_win allows
        size_t maxLen = std::min(_send_buffer->size(), tcp_win);
        if (_contentLength) {
          maxLen = _contentLength > _sentLength ? std::min(maxLen, _contentLength - _sentLength) : 0;
        }

        size_t const readLen = _fillBufferAndProcessTemplates(_send_buffer->data(), maxLen);

        if (readLen == 0) {
          // no more data to send
          _state = RESPONSE_END;
        } else if (readLen != RESPONSE_TRY_AGAIN) {
          _send_buffer_len += readLen;  // set buffers's size to match added data
          _sentLength += readLen;       // data is not sent yet, but we need it to understand that it would be last block
          if (_sendContentLength && (_sentLength == _contentLength)) {
            // it was last piece of content
            _state = RESPONSE_END;
          }
        }
      }
    } while (_send_buffer_len);  // go on till we have something in buffer pending to send

    // execute sending whatever we have in sock buffs now
    request->client()->send();
    _writtenLength += payloadlen;
#if ASYNCWEBSERVER_USE_CHUNK_INFLIGHT
    _in_flight += payloadlen;
    --_in_flight_credit;  // take a credit
#endif
    if (_send_buffer_len == 0) {
      // buffer empty, we can release mem, otherwise need to keep it till next run (should not happen under normal conditions)
      _send_buffer.reset();
    }
    return payloadlen;
  }  // (_state == RESPONSE_CONTENT)

  // implicit check
  if (_state == RESPONSE_WAIT_ACK) {
    // we do not need to wait for any acks actually if we won't send any more data,
    // connection would be closed gracefully with last piece of data (in AsyncWebServerRequest::_onAck)
    _state = RESPONSE_END;
  }
  return 0;
}

