#pragma once
#include "MqttTransport.h"
#include <PubSubClient.h>

/**
 * @defgroup transport MQTT Transports
 * @brief Transport adapters for different MQTT client libraries.
 * @{
 */

/**
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
   * @brief Set MQTT server and credentials.
   *
   * @param host MQTT host/IP
   * @param port MQTT port
   * @param user MQTT username
   * @param pass MQTT password
   */
  void setServer(const char* host, uint16_t port, const char* user = nullptr, const char* pass = nullptr) override {
    client.setServer(host, port);
    this->user = user;
    this->pass = pass;
  }

  /**
   * @brief Set MQTT server and credentials (std::string version).
   *
   * @param host MQTT host/IP
   * @param port MQTT port
   * @param user MQTT username
   * @param pass MQTT password
   */
  void setServer(const std::string& host, uint16_t port, const std::string& user = "", const std::string& pass = "") override {
    client.setServer(host.c_str(), port);
    this->userStr = user;
    this->passStr = pass;
    this->user = this->userStr.c_str();
    this->pass = this->passStr.c_str();
  }

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

    if (log) log->debug("PubSub publish topic=%s len=%u retained=%d", topic,
                        (unsigned)len, retained ? 1 : 0);

    bool ok = client.publish(topic,
                             payload,
                             static_cast<unsigned int>(len),
                             retained);

    if (!ok) {
      if (log) log->error("PubSub publish FAILED topic=%s", topic);
    } else {
      if (log) log->debug("PubSub publish OK topic=%s", topic);
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
  const char* user = nullptr;
  const char* pass = nullptr;
  std::string userStr;
  std::string passStr;
  bool wasConnected = false;
  void (*cb)(void*) = nullptr;
  /** @brief Pointer to user context for callback. */
  void* ctx = nullptr;
};
/** @} */
