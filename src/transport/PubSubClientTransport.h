#pragma once
#include "MqttTransport.h"
#include <PubSubClient.h>

/**
 * @defgroup transport MQTT Transports
 * @brief Transport adapters for different MQTT client libraries.
 * @{
 * @brief MQTT transport adapter for PubSubClient.
 *
 * PubSubClient is a synchronous, polling-based MQTT client.
 *
 * Limitations:
 * - The host firmware MUST call mqtt.loop() frequently.
 * - QoS handling is best-effort.
 * - Connection events are detected via rising-edge logic in tick().
 */
class PubSubClientTransport : public MqttTransport {
public:
  /**
   * @brief Construct a PubSubClient transport adapter.
   *
   * @param client Reference to an already configured PubSubClient instance
   */
  explicit PubSubClientTransport(PubSubClient& client)
    : client(client) {}

  /**
   * @inheritdoc
   */
  bool connected() const override {
    return client.connected();
  }

  /**
   * @inheritdoc
   */
  bool publish(const char* topic,
               const uint8_t* payload,
               size_t len,
               bool retained,
               uint8_t /*qos*/) override {
    if (payload == nullptr) {
      len = 0;
    }

    JB_LOGD("MQTT", "PubSub publish topic=%s len=%u retained=%d", topic,
           (unsigned)len, retained ? 1 : 0);

    bool ok = client.publish(topic,
                             payload,
                             static_cast<unsigned int>(len),
                             retained);

    if (!ok) {
      JB_LOGE("MQTT", "PubSub publish FAILED topic=%s", topic);
    } else {
      JB_LOGD("MQTT", "PubSub publish OK topic=%s", topic);
    }
    return ok;
  }

  /**
   * @inheritdoc
   */
  void setOnConnect(void (*cb_)(void*), void* ctx_) override {
    cb = cb_;
    ctx = ctx_;
  }

  /**
   * @brief Detect MQTT connection transitions.
   *
   * This method must be called periodically by the host firmware.
   *
   * It detects a rising-edge transition (disconnected -> connected) and
   * invokes the registered onConnect callback if present.
   *
   * @note This does NOT replace PubSubClient::loop().
   */
  void tick() override {
    bool nowConnected = client.connected();

    if (nowConnected && !wasConnected) {
      if (cb) {
        cb(ctx);
      }
    }

    wasConnected = nowConnected;
  }

private:
  PubSubClient& client;
  bool wasConnected = false;
  void (*cb)(void*) = nullptr;
  void* ctx = nullptr;
};
