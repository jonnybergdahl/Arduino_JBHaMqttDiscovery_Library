#pragma once
#include "MqttTransport.h"
#include <Arduino.h>

/**
 * @defgroup transport MQTT Transports
 * @brief Transport adapters for different MQTT client libraries.
 * @{
 */

/**
 * @brief MQTT transport adapter for espMqttClient (https://github.com/bertmelis/espMqttClient).
 *
 * espMqttClient is a maintained successor of AsyncMqttClient with the same style of API.
 * This transport works with all its client classes; pass the class as the template
 * parameter and include its header yourself:
 *
 * @code
 * #include <espMqttClient.h>          // espMqttClient, espMqttClientSecure
 * #include <espMqttClientAsync.h>     // espMqttClientAsync (needs AsyncTCP / ESPAsyncTCP)
 * #include <transport/EspMqttClientTransport.h>
 *
 * espMqttClient mqtt;
 * EspMqttClientTransport<espMqttClient> transport(mqtt);
 * @endcode
 *
 * Who runs the client's loop():
 * - espMqttClient on ESP8266: nobody else, so tick() calls it (callLoop = true, the default there).
 * - espMqttClient on ESP32: its own task by default, so tick() must not (callLoop = false,
 *   the default there). If you create it with `espMqttClientTypes::UseInternalTask::NO`,
 *   pass callLoop = true.
 * - espMqttClientAsync: its network callbacks drive it, so pass callLoop = false on ESP8266.
 *
 * espMqttClient keeps pointers to the host, client id, credentials and will instead of
 * copying them. This transport keeps its own copies, so callers don't need to keep
 * those strings alive.
 *
 * The transport takes over the client's onConnect, onDisconnect and onMessage callbacks
 * (espMqttClient keeps only one of each by default).
 *
 * After the first connect(), tick() reconnects when the connection drops (see
 * setAutoReconnect()).
 *
 * @tparam Client espMqttClient, espMqttClientSecure or espMqttClientAsync
 */
template <class Client>
class EspMqttClientTransport : public MqttTransport {
public:
  // Keep the base class overloads (Arduino String) visible next to the overrides
  using MqttTransport::setServer;
  using MqttTransport::connect;
  using MqttTransport::subscribe;
  using MqttTransport::publish;

  /** @brief How long a connect attempt may take before tick() gives up on it. */
  static constexpr uint32_t CONNECT_TIMEOUT_MS = 30000;

  /** @brief reconnectNow() drops a pending attempt only if it's older than this. */
  static constexpr uint32_t STALE_ATTEMPT_MS = 2000;

#if defined(ESP8266)
  static constexpr bool DEFAULT_CALL_LOOP = true;
#else
  static constexpr bool DEFAULT_CALL_LOOP = false;
#endif

  /**
   * @brief Construct an espMqttClient transport adapter.
   *
   * @param client   The espMqttClient instance
   * @param callLoop Whether tick() calls client.loop() (see the class description)
   */
  explicit EspMqttClientTransport(Client& client, bool callLoop = DEFAULT_CALL_LOOP)
    : client(client), callLoop(callLoop) {}

  /**
   * @brief Set MQTT server and credentials. All strings are copied.
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
   * Starts the connection and returns right away. The onConnect callback fires when
   * it's established. If it fails, tick() keeps trying (see setAutoReconnect()).
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
   * @inheritdoc
   */
  bool connected() const override {
    return isConnected;
  }

  /**
   * @inheritdoc
   */
  bool publish(const char* topic, const uint8_t* payload, size_t len, bool retained, uint8_t qos) override {
    if (!isConnected) {
      // espMqttClient would queue it, but HaDiscovery stores and re-sends on connect itself
      if (log) log->warning("espMqtt publish skipped (disconnected) topic=%s", topic);
      return false;
    }
    static const uint8_t empty = 0;
    uint16_t pid = client.publish(topic, qos, retained, payload ? payload : &empty, payload ? len : 0);
    if (pid == 0) {
      // Also happens when free memory is below EMC_MIN_FREE_MEMORY
      if (log) log->error("espMqtt publish FAILED topic=%s len=%u", topic, (unsigned)len);
      return false;
    }
    if (log) log->debug("espMqtt publish OK topic=%s len=%u pid=%u", topic, (unsigned)len, (unsigned)pid);
    return true;
  }

  /**
   * @inheritdoc
   */
  bool subscribe(const char* topic, uint8_t qos = 0) override {
    if (!isConnected) {
      if (log) log->warning("espMqtt subscribe skipped (disconnected) topic=%s", topic);
      return false;
    }
    uint16_t pid = client.subscribe(topic, qos);
    if (pid == 0) {
      if (log) log->error("espMqtt subscribe FAILED topic=%s", topic);
      return false;
    }
    if (log) log->debug("espMqtt subscribe OK topic=%s pid=%u", topic, (unsigned)pid);
    return true;
  }

  /**
   * @inheritdoc
   *
   * Messages split into several parts are ignored. Discovery command payloads are short.
   */
  void setOnMessage(MessageCallback cb_, void* ctx_) override {
    msgCb = cb_;
    msgCtx = ctx_;
    client.onMessage([this](const espMqttClientTypes::MessageProperties& /*properties*/, const char* topic,
                            const uint8_t* payload, size_t len, size_t index, size_t total) {
      if (index != 0 || len != total) {
        if (log) log->warning("espMqtt message on %s split into parts (%u of %u bytes), ignored",
                              topic, (unsigned)len, (unsigned)total);
        return;
      }
      if (msgCb) {
        msgCb(msgCtx, topic, payload, len);
      }
    });
  }

  /**
   * @inheritdoc
   */
  void setWill(const char* topic, const char* payload, bool retained = true, uint8_t qos = 1) override {
    willTopicStr = topic ? topic : "";
    willPayloadStr = payload ? payload : "";
    client.setWill(willTopicStr.c_str(), qos, retained,
                   reinterpret_cast<const uint8_t*>(willPayloadStr.data()), willPayloadStr.size());
  }

  /**
   * @inheritdoc
   */
  void setOnConnect(void (*cb_)(void*), void* ctx_) override {
    cb = cb_;
    ctx = ctx_;

    // These may run in the client's or the network task; tick() acts on the flags from loop()
    client.onConnect([this](bool /*sessionPresent*/) {
      isConnected = true;
      connecting = false;
      if (cb) {
        cb(ctx);
      }
    });

    client.onDisconnect([this](espMqttClientTypes::DisconnectReason reason) {
      lastReason = static_cast<uint8_t>(reason);
      droppedSession = isConnected;
      isConnected = false;
      connecting = false;
      disconnectEvent = true;
    });
  }

  /**
   * @brief Run the client's loop() (if callLoop) and automatic reconnect.
   *
   * Called from HaDiscovery::tick().
   */
  void tick() override {
    if (callLoop) {
      client.loop();
    }
    if (!started) {
      return;
    }
    uint32_t now = millis();

    if (disconnectEvent) {
      disconnectEvent = false;
      if (droppedSession) {
        if (log) log->warning("espMqtt connection lost, reason %u", (unsigned)lastReason);
        reconnect.reset(now);   // try again right away
      } else if (!attemptFailed) {
        if (log) log->error("espMqtt connect FAILED, reason %u", (unsigned)lastReason);
        attemptFailed = true;
        reconnect.failed(now);  // wait before the next attempt
      }
    }

    if (takeReconnectRequest() && !isConnected) {
      if (connecting && static_cast<int32_t>(now - attemptStartedAt) >= static_cast<int32_t>(STALE_ATTEMPT_MS)) {
        if (log) log->info("espMqtt reconnect requested, dropping the current attempt");
        connecting = false;
        attemptFailed = true;
        client.disconnect(true);
      }
      reconnect.reset(now);
    }

    if (connecting && static_cast<int32_t>(now - attemptStartedAt) >= static_cast<int32_t>(CONNECT_TIMEOUT_MS)) {
      // Safety net: espMqttClient normally reports a failed attempt itself
      if (log) log->error("espMqtt connect timed out, resetting the client");
      connecting = false;
      client.disconnect(true);
      if (!attemptFailed) {
        attemptFailed = true;
        reconnect.failed(now);
      }
    }

    // Only start a new attempt once the client is fully disconnected
    if (!isConnected && !connecting && client.disconnected() && reconnect.due(now)) {
      if (log) log->info("espMqtt reconnecting");
      startAttempt();
    }
  }

private:
  void startAttempt() {
    if (log) log->info("espMqtt connecting");
    attemptStartedAt = millis();
    attemptFailed = false;
    // Set before connect(): onConnect may fire in another task before connect() returns
    connecting = true;
    if (!client.connect()) {
      connecting = false;
      // Not in a state to connect yet (e.g. still disconnecting); tick() retries later
      attemptFailed = true;
      reconnect.failed(attemptStartedAt);
    }
  }

  Client& client;
  bool callLoop;
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
  volatile uint8_t lastReason = 0;
  bool started = false;
  bool attemptFailed = false;
  uint32_t attemptStartedAt = 0;
  void (*cb)(void*) = nullptr;
  void* ctx = nullptr;
  MessageCallback msgCb = nullptr;
  void* msgCtx = nullptr;
};
/** @} */
