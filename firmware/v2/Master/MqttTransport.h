#pragma once
// The broker connection whose writes never wait (#630). espMqttClient's own
// ClientSync writes through NetworkClient::write, which waits for the peer to
// take the bytes with a budget that starts over at every partial send: on a
// link that drips, one write has no end, and the mqtt task feeds the task
// watchdog only between its passes. This transport hands the socket what it
// takes now and returns the count; MqttClient keeps its place in the packet
// (_bytesSent) and sends the rest on its next pass. The same change is open
// upstream as bertmelis/espMqttClient#191.
#include <espMqttClient.h>

class MqttNonBlockingTransport : public espMqttClientInternals::ClientSync {
 public:
  size_t write(const uint8_t* buf, size_t size) override;
};

// espMqttClient on that transport, pumped from the mqtt task (no internal task).
class MqttWallClient : public MqttClientSetup<MqttWallClient> {
 public:
  MqttWallClient();

 protected:
  MqttNonBlockingTransport _client;
};
