#pragma once
#include "MqttTransport.h"
#include <PubSubClient.h>
#include <string.h>

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
 * - PubSubClient's default 256 byte packet buffer is too small for discovery
 *   payloads. This transport raises it to at least DEFAULT_BUFFER_SIZE in
 *   setServer(), and grows it further if a publish needs more.
 * - QoS handling is best-effort (publish is always QoS 0, subscribe supports 0 and 1).
 * - Connection events are detected via rising-edge logic in tick().
 * - The transport takes over PubSubClient's message callback.
 */
class PubSubClientTransport : public MqttTransport {
public:
  /** @brief Default minimum PubSubClient buffer size in bytes. */
  static constexpr uint16_t DEFAULT_BUFFER_SIZE = 1024;

  /** @brief Bytes PubSubClient needs besides topic and payload (header + topic length). */
  static constexpr size_t PACKET_OVERHEAD = 7;

  /**
   * @brief Construct a PubSubClient transport adapter.
   *
   * The buffer is not resized here, since this usually runs as a global constructor.
   * It is resized in setServer().
   *
   * @param client     Reference to an already configured PubSubClient instance
   * @param bufferSize Minimum PubSubClient buffer size in bytes (default DEFAULT_BUFFER_SIZE)
   */
  explicit PubSubClientTransport(PubSubClient& client, uint16_t bufferSize = DEFAULT_BUFFER_SIZE)
    : client(client), minBufferSize(bufferSize) {}

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
    // PubSubClient keeps the host pointer, so it must point at our own copy
    hostStr = host;
    userStr = user;
    passStr = pass;
    client.setServer(hostStr.c_str(), port);
    ensureBufferSize(minBufferSize);
  }

  /**
   * @inheritdoc
   *
   * Blocks until the broker answers or the connection fails.
   */
  bool connect(const char* clientId = nullptr) override {
    if (!clientId || !*clientId) {
      if (log) log->error("PubSub connect needs a client id");
      return false;
    }

    const char* user = userStr.empty() ? nullptr : userStr.c_str();
    const char* pass = passStr.empty() ? nullptr : passStr.c_str();
    const char* willTopic = willTopicStr.empty() ? nullptr : willTopicStr.c_str();
    const char* willMessage = willTopic ? willPayloadStr.c_str() : nullptr;

    if (log) log->info("PubSub connecting as %s", clientId);
    bool ok = client.connect(clientId, user, pass, willTopic, willQos, willRetain, willMessage);
    if (!ok) {
      if (log) log->error("PubSub connect FAILED, state=%d", client.state());
    }
    return ok;
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

    size_t needed = PACKET_OVERHEAD + strlen(topic) + len;
    if (needed > client.getBufferSize()) {
      if (log) log->warning("PubSub buffer too small (%u < %u), growing it", (unsigned)client.getBufferSize(), (unsigned)needed);
      if (!ensureBufferSize(needed)) {
        if (log) log->error("PubSub publish FAILED topic=%s, could not grow buffer to %u bytes", topic, (unsigned)needed);
        return false;
      }
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
  bool subscribe(const char* topic, uint8_t qos = 0) override {
    bool ok = client.subscribe(topic, qos > 1 ? 1 : qos);
    if (log) {
      if (ok) {
        log->debug("PubSub subscribe OK topic=%s", topic);
      } else {
        log->error("PubSub subscribe FAILED topic=%s", topic);
      }
    }
    return ok;
  }

  /**
   * @inheritdoc
   *
   * This replaces any callback set directly with PubSubClient::setCallback().
   */
  void setOnMessage(MessageCallback cb_, void* ctx_) override {
    msgCb = cb_;
    msgCtx = ctx_;
#if defined(ESP8266) || defined(ESP32)
    client.setCallback([this](char* topic, uint8_t* payload, unsigned int len) {
      handleMessage(topic, payload, len);
    });
#else
    // Plain function pointer callback, so only one instance can receive messages
    instance() = this;
    client.setCallback(&PubSubClientTransport::messageThunk);
#endif
  }

  /**
   * @inheritdoc
   */
  void setWill(const char* topic, const char* payload, bool retained = true, uint8_t qos = 1) override {
    willTopicStr = topic ? topic : "";
    willPayloadStr = payload ? payload : "";
    willRetain = retained;
    willQos = qos > 1 ? 1 : qos;
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
  void handleMessage(const char* topic, const uint8_t* payload, unsigned int len) {
    if (log) log->debug("PubSub message topic=%s len=%u", topic, len);
    if (msgCb) {
      msgCb(msgCtx, topic, payload, len);
    }
  }

#if !(defined(ESP8266) || defined(ESP32))
  static PubSubClientTransport*& instance() {
    static PubSubClientTransport* inst = nullptr;
    return inst;
  }

  static void messageThunk(char* topic, uint8_t* payload, unsigned int len) {
    if (instance()) {
      instance()->handleMessage(topic, payload, len);
    }
  }
#endif

  /**
   * @brief Grow the PubSubClient buffer to at least @p size bytes. Never shrinks it.
   *
   * @return true if the buffer is now at least @p size bytes
   */
  bool ensureBufferSize(size_t size) {
    if (size > UINT16_MAX) {
      return false;
    }
    if (client.getBufferSize() >= size) {
      return true;
    }
    return client.setBufferSize(static_cast<uint16_t>(size));
  }

  PubSubClient& client;
  uint16_t minBufferSize;
  std::string hostStr;
  std::string userStr;
  std::string passStr;
  std::string willTopicStr;
  std::string willPayloadStr;
  bool willRetain = true;
  uint8_t willQos = 1;
  bool wasConnected = false;
  void (*cb)(void*) = nullptr;
  /** @brief Pointer to user context for callback. */
  void* ctx = nullptr;
  MessageCallback msgCb = nullptr;
  void* msgCtx = nullptr;
};
/** @} */
