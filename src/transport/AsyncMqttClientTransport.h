#pragma once
#include "MqttTransport.h"
#include "MessageAssembler.h"
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
 * AsyncMqttClient hands over longer incoming messages in parts. The transport puts
 * them together up to setMaxMessageSize() bytes (4096 by default) and drops larger ones.
 *
 * After the first connect(), tick() reconnects when the connection drops (see
 * setAutoReconnect()). Turn this off if your firmware already reconnects with its
 * own timer, as many AsyncMqttClient sketches do.
 *
 * AsyncMqttClient ignores a TCP connect that fails right away (e.g. while Wi-Fi is
 * down): no callback fires and the client stays in its connecting state, ignoring
 * further connect() calls. tick() therefore gives up on an attempt after
 * CONNECT_TIMEOUT_MS, resets the client and tries again later.
 */
class AsyncMqttClientTransport : public MqttTransport {
public:
  // Keep the base class overloads (Arduino String) visible next to the overrides
  using MqttTransport::setServer;
  using MqttTransport::connect;
  using MqttTransport::subscribe;
  using MqttTransport::publish;

  /** @brief How long a connect attempt may take before tick() gives up on it. */
  static constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;

  /** @brief reconnectNow() drops a pending attempt only if it's older than this. */
  static constexpr uint32_t STALE_ATTEMPT_MS = 2000;

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
    takeReconnectRequest();  // this is the attempt a pending request asked for
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
      } else if (!attemptFailed) {
        if (log) log->error("Async connect FAILED");
        attemptFailed = true;
        reconnect.failed(now);  // wait before the next attempt
      }
    }

    if (takeReconnectRequest() && !isConnected) {
      if (connecting && static_cast<int32_t>(now - attemptStartedAt) >= static_cast<int32_t>(STALE_ATTEMPT_MS)) {
        // An attempt started while the network was down may never call back,
        // so drop it instead of waiting for the timeout. A fresh one is left alone.
        if (log) log->info("Async reconnect requested, dropping the current attempt");
        connecting = false;
        attemptFailed = true;  // a late onDisconnect for it isn't a new failure
        client.disconnect(true);
      }
      reconnect.reset(now);
    }

    if (connecting && static_cast<int32_t>(now - attemptStartedAt) >= static_cast<int32_t>(CONNECT_TIMEOUT_MS)) {
      // No onConnect or onDisconnect came: reset the client so connect() works again
      if (log) log->error("Async connect timed out, resetting the client");
      connecting = false;
      client.disconnect(true);
      if (!attemptFailed) {
        attemptFailed = true;
        reconnect.failed(now);
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
      const uint8_t* data = reinterpret_cast<const uint8_t*>(payload);
      if (index == 0 && len == total) {
        deliver(topic, data, len);
        return;
      }
      switch (assembler.add(topic, data, len, index, total)) {
        case MessageAssembler::Result::Complete:
          deliver(assembler.topic().c_str(), reinterpret_cast<const uint8_t*>(assembler.payload().data()),
                  assembler.payload().size());
          assembler.release();
          break;
        case MessageAssembler::Result::TooLarge:
          if (log) log->warning("Async message on %s is %u bytes, more than %u, ignored",
                                topic, (unsigned)total, (unsigned)assembler.maxSize());
          break;
        case MessageAssembler::Result::Broken:
          if (log) log->warning("Async message on %s arrived out of order, ignored", topic);
          break;
        default:
          break;
      }
    });
  }

  /**
   * @brief Set the largest incoming message that is put together from parts.
   *
   * AsyncMqttClient hands over longer messages in parts; larger ones are dropped.
   *
   * @param maxSize Size in bytes (default 4096)
   */
  void setMaxMessageSize(size_t maxSize) {
    assembler.setMaxSize(maxSize);
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
  void deliver(const char* topic, const uint8_t* payload, size_t len) {
    if (log) log->debug("Async message topic=%s len=%u", topic, (unsigned)len);
    if (msgCb) {
      msgCb(msgCtx, topic, payload, len);
    }
  }

  void startAttempt() {
    if (log) log->info("Async connecting");
    attemptStartedAt = millis();
    attemptFailed = false;
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
  bool attemptFailed = false;
  uint32_t attemptStartedAt = 0;
  void (*cb)(void*) = nullptr;
  /** @brief Pointer to user context for callback. */
  void* ctx = nullptr;
  MessageCallback msgCb = nullptr;
  MessageAssembler assembler;
  void* msgCtx = nullptr;
};
/** @} */
