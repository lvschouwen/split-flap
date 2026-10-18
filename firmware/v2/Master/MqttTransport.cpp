#include "MqttTransport.h"

#include <errno.h>
#include <lwip/sockets.h>

size_t MqttNonBlockingTransport::write(const uint8_t* buf, size_t size) {
  const int fd = client.fd();
  if (fd < 0 || size == 0) return 0;
  const ssize_t sent = lwip_send(fd, buf, size, MSG_DONTWAIT);
  if (sent >= 0) return (size_t)sent;
  // A full send buffer is the next pass's business; anything else ends the
  // connection, and the client reconnects on its backoff.
  if (errno != EAGAIN && errno != EWOULDBLOCK) client.stop();
  return 0;
}

MqttWallClient::MqttWallClient()
    : MqttClientSetup(espMqttClientTypes::UseInternalTask::NO), _client() {
  _transport = &_client;
}
