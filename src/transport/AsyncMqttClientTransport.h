#pragma once
#include "MqttTransport.h"
#include <AsyncMqttClient.h>

/**
 * @defgroup transport MQTT Transports
 * @brief Transport adapters for different MQTT client libraries.
 * @{
 * @brief MQTT transport adapter for AsyncMqttClient.
 *
 * AsyncMqttClient is a fully asynchronous, event-driven MQTT client.
 *
 * Advantages:
 * - No polling required
 * - Proper QoS support
 * - Native connection callbacks
 *
 * This transport provides the most reliable behavior for Home Assistant
 * discovery and availability handling.
 */
class AsyncMqttClientTransport : public MqttTransport {
public:
  /**
   * @brief Construct an AsyncMqttClient transport adapter.
   *
   * @param client Reference to an already configured AsyncMqttClient instance
   */
  explicit AsyncMqttClientTransport(AsyncMqttClient& client)
    : client(client) {}

  /**
   * @inheritdoc
   */
  bool connected() const override {
    return isConnected;
  }

  /**
   * @inheritdoc
   */
  bool publish(const char* topic,
               const uint8_t* payload,
               size_t len,
               bool retained,
               uint8_t qos) override {
    if (!isConnected) {
      JB_LOGW("MQTT", "Async publish skipped (disconnected) topic=%s", topic);
      return false;
    }

    const char* p = payload ? reinterpret_cast<const char*>(payload) : "";
    size_t l = payload ? len : 0;

    JB_LOGD("MQTT", "Async publish topic=%s len=%u retained=%d qos=%u", topic,
           (unsigned)l, retained ? 1 : 0, (unsigned)qos);

    uint16_t pid = client.publish(topic, qos, retained, p, l);
    if (pid == 0) {
      JB_LOGE("MQTT", "Async publish FAILED topic=%s", topic);
    } else {
      JB_LOGD("MQTT", "Async publish OK topic=%s pid=%u", topic, (unsigned)pid);
    }
    return true;
  }

  /**
   * @inheritdoc
   */
  void setOnConnect(void (*cb_)(void*), void* ctx_) override {
    cb = cb_;
    ctx = ctx_;

    client.onConnect([this](bool /*sessionPresent*/) {
      isConnected = true;
      if (cb) {
        cb(ctx);
      }
    });

    client.onDisconnect([this](AsyncMqttClientDisconnectReason /*reason*/) {
      isConnected = false;
    });
  }

private:
  AsyncMqttClient& client;
  volatile bool isConnected = false;
  void (*cb)(void*) = nullptr;
  void* ctx = nullptr;
};
/** @} */
