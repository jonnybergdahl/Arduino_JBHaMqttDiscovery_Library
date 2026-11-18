#pragma once
#include "MqttTransport.h"
#include <AsyncMqttClient.h>

/**
 * @defgroup transport MQTT Transports
 * @brief Transport adapters for different MQTT client libraries.
 * @{
 */

/**
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
 *
 * AsyncMqttClient keeps pointers to the host, client id, credentials and will
 * instead of copying them. This transport keeps its own copies, so callers
 * don't need to keep those strings alive.
 *
 * Incoming messages split over several TCP packets are ignored. Discovery
 * command payloads are short, so this doesn't happen in practice.
 *
 * After the first connect(), tick() reconnects when the connection drops (see
 * setAutoReconnect()). Turn this off if your firmware already reconnects with its
 * own timer, as many AsyncMqttClient sketches do.
 */
class AsyncMqttClientTransport : public MqttTransport {
public:
  /**
   * @brief Construct an AsyncMqttClient transport adapter.
   *
   * @param client Reference to an AsyncMqttClient instance
   */
  explicit AsyncMqttClientTransport(AsyncMqttClient& client)
    : client(client) {}

  /**
   * @brief Set MQTT server and credentials.
   *
   * All strings are copied.
   *
   * @param host MQTT host/IP
   * @param port MQTT port
   * @param user MQTT username (optional)
   * @param pass MQTT password (optional)
   */
  void setServer(const char* host, uint16_t port, const char* user = nullptr, const char* pass = nullptr) override {
    setServer(std::string(host ? host : ""), port,
              std::string(user ? user : ""), std::string(pass ? pass : ""));
  }

  /**
   * @brief Set MQTT server and credentials (std::string version).
   *
   * @param host MQTT host/IP
   * @param port MQTT port
   * @param user MQTT username (empty for none)
   * @param pass MQTT password (empty for none)
   */
  void setServer(const std::string& host, uint16_t port, const std::string& user = "", const std::string& pass = "") override {
    hostStr = host;
    userStr = user;
    passStr = pass;
    client.setServer(hostStr.c_str(), port);
    if (!userStr.empty()) {
      client.setCredentials(userStr.c_str(), passStr.empty() ? nullptr : passStr.c_str());
    }
  }

  /**
   * @inheritdoc
   *
   * Starts the connection and returns right away. The onConnect callback
   * fires when the connection is established. If it fails, tick() keeps trying
   * (see setAutoReconnect()).
   */
  bool connect(const char* clientId = nullptr) override {
    if (clientId && *clientId && clientId != clientIdStr.c_str()) {
      clientIdStr = clientId;
      client.setClientId(clientIdStr.c_str());
    }
    started = true;
    startAttempt();
    return true;
  }

  /**
   * @brief Run automatic reconnect. Called from HaDiscovery::tick().
   */
  void tick() override {
    if (!started) {
      return;
    }
    uint32_t now = millis();

    if (disconnectEvent) {
      disconnectEvent = false;
      if (droppedSession) {
        if (log) log->warning("Async connection lost");
        reconnect.reset(now);   // try again right away
      } else {
        if (log) log->error("Async connect FAILED");
        reconnect.failed(now);  // wait before the next attempt
      }
    }

    if (!isConnected && !connecting && reconnect.due(now)) {
      if (log) log->info("Async reconnecting");
      startAttempt();
    }
  }

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
      if (log) log->warning("Async publish skipped (disconnected) topic=%s", topic);
      return false;
    }

    const char* p = payload ? reinterpret_cast<const char*>(payload) : "";
    size_t l = payload ? len : 0;

    if (log) log->debug("Async publish topic=%s len=%u retained=%d qos=%u", topic,
                        (unsigned)l, retained ? 1 : 0, (unsigned)qos);

    // Returns the packet id (1 for QoS 0), or 0 on failure
    uint16_t pid = client.publish(topic, qos, retained, p, l);
    if (pid == 0) {
      if (log) log->error("Async publish FAILED topic=%s", topic);
      return false;
    }
    if (log) log->debug("Async publish OK topic=%s pid=%u", topic, (unsigned)pid);
    return true;
  }

  /**
   * @inheritdoc
   */
  bool subscribe(const char* topic, uint8_t qos = 0) override {
    if (!isConnected) {
      if (log) log->warning("Async subscribe skipped (disconnected) topic=%s", topic);
      return false;
    }
    uint16_t pid = client.subscribe(topic, qos);
    if (pid == 0) {
      if (log) log->error("Async subscribe FAILED topic=%s", topic);
      return false;
    }
    if (log) log->debug("Async subscribe OK topic=%s pid=%u", topic, (unsigned)pid);
    return true;
  }

  /**
   * @inheritdoc
   *
   * Call this only once; AsyncMqttClient adds a new handler on every call.
   */
  void setOnMessage(MessageCallback cb_, void* ctx_) override {
    msgCb = cb_;
    msgCtx = ctx_;

    client.onMessage([this](char* topic, char* payload, AsyncMqttClientMessageProperties /*properties*/,
                            size_t len, size_t index, size_t total) {
      if (index != 0 || len != total) {
        if (log) log->warning("Async message on %s split into parts (%u of %u bytes), ignored",
                              topic, (unsigned)len, (unsigned)total);
        return;
      }
      if (log) log->debug("Async message topic=%s len=%u", topic, (unsigned)len);
      if (msgCb) {
        msgCb(msgCtx, topic, reinterpret_cast<const uint8_t*>(payload), len);
      }
    });
  }

  /**
   * @inheritdoc
   */
  void setWill(const char* topic, const char* payload, bool retained = true, uint8_t qos = 1) override {
    willTopicStr = topic ? topic : "";
    willPayloadStr = payload ? payload : "";
    client.setWill(willTopicStr.c_str(), qos, retained, willPayloadStr.c_str(), willPayloadStr.size());
  }

  /**
   * @inheritdoc
   *
   * Call this only once; AsyncMqttClient adds new handlers on every call.
   */
  void setOnConnect(void (*cb_)(void*), void* ctx_) override {
    cb = cb_;
    ctx = ctx_;

    // These run in the network task; tick() acts on the flags from loop()
    client.onConnect([this](bool /*sessionPresent*/) {
      isConnected = true;
      connecting = false;
      if (cb) {
        cb(ctx);
      }
    });

    client.onDisconnect([this](AsyncMqttClientDisconnectReason /*reason*/) {
      droppedSession = isConnected;
      isConnected = false;
      connecting = false;
      disconnectEvent = true;
    });
  }

private:
  void startAttempt() {
    if (log) log->info("Async connecting");
    connecting = true;
    client.connect();
  }

  AsyncMqttClient& client;
  std::string hostStr;
  std::string userStr;
  std::string passStr;
  std::string clientIdStr;
  std::string willTopicStr;
  std::string willPayloadStr;
  volatile bool isConnected = false;
  volatile bool connecting = false;
  volatile bool disconnectEvent = false;
  volatile bool droppedSession = false;
  bool started = false;
  void (*cb)(void*) = nullptr;
  /** @brief Pointer to user context for callback. */
  void* ctx = nullptr;
  MessageCallback msgCb = nullptr;
  void* msgCtx = nullptr;
};
/** @} */
