#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#ifdef ARDUINO
#include <Arduino.h>  // String
#endif

#ifndef NO_JB_LOGGER
#include <jblogger.h>
#else
#include "JBLoggerDummy.h"
#endif

/**
 * @brief Delay between reconnect attempts, doubling after each failure.
 *
 * Time is passed in, so this works the same on Arduino (millis()) and in native tests.
 */
struct ReconnectBackoff {
  /** @brief Whether automatic reconnect is enabled. */
  bool enabled = true;
  /** @brief Delay after the first failed attempt, in milliseconds. */
  uint32_t minDelayMs = 1000;
  /** @brief Longest delay between attempts, in milliseconds. */
  uint32_t maxDelayMs = 60000;
  /** @brief Delay used after the next failure. */
  uint32_t currentDelayMs = 1000;
  /** @brief Time of the next allowed attempt. */
  uint32_t nextAttemptAt = 0;

  /** @brief Start over: allow an attempt right away, and start the delay at minDelayMs. */
  void reset(uint32_t now) {
    currentDelayMs = minDelayMs;
    nextAttemptAt = now;
  }

  /** @brief Whether an attempt is allowed at @p now. */
  bool due(uint32_t now) const {
    return enabled && static_cast<int32_t>(now - nextAttemptAt) >= 0;
  }

  /** @brief Record a failed attempt and schedule the next one. */
  void failed(uint32_t now) {
    nextAttemptAt = now + currentDelayMs;
    currentDelayMs = currentDelayMs >= maxDelayMs / 2 ? maxDelayMs : currentDelayMs * 2;
  }
};

/**
 * @defgroup transport MQTT Transports
 * @brief Transport adapters for different MQTT client libraries.
 * @{
 */
/**
 * @brief Abstract MQTT transport interface.
 *
 * This interface decouples the Home Assistant Discovery logic from the
 * underlying MQTT client implementation.
 *
 * It supports both:
 * - synchronous / polling MQTT clients (e.g. PubSubClient)
 * - asynchronous / event-driven MQTT clients (e.g. AsyncMqttClient)
 *
 * The transport is responsible only for:
 * - connecting with the configured server, credentials and Last Will
 * - reporting connection state
 * - publishing MQTT messages
 * - subscribing to topics and passing incoming messages on
 * - notifying when a connection is (re)established
 *
 * Transports copy every string passed to them, so callers don't need to keep
 * them alive.
 *
 * It does NOT manage Wi-Fi.
 *
 * After the first connect(), transports reconnect by themselves from tick() when the
 * connection drops, waiting longer after each failed attempt. Turn this off with
 * setAutoReconnect(false) if your firmware reconnects itself.
 */
class MqttTransport {
public:
  /**
   * @brief Callback for incoming messages.
   *
   * @param ctx     User context pointer given to setOnMessage()
   * @param topic   Topic the message arrived on (null-terminated)
   * @param payload Payload bytes (not null-terminated)
   * @param len     Payload length in bytes
   */
  using MessageCallback = void (*)(void* ctx, const char* topic, const uint8_t* payload, size_t len);

  virtual ~MqttTransport() = default;

  /**
   * @brief Connect to the MQTT server set with setServer().
   *
   * Uses the credentials from setServer() and the Last Will from setWill().
   * For asynchronous transports this only starts the connection; the
   * onConnect callback fires when it's established.
   *
   * @param clientId MQTT client id. Required by synchronous transports.
   *                 Asynchronous transports generate one if nullptr.
   * @return true if the connection was established (sync) or started (async)
   */
  virtual bool connect(const char* clientId = nullptr) = 0;

  /**
   * @brief Subscribe to a topic.
   *
   * Subscriptions don't survive a reconnect with a clean session, so subscribe
   * again from the onConnect callback.
   *
   * @param topic MQTT topic (null-terminated string)
   * @param qos   Requested QoS level (0 or 1)
   * @return true if the subscribe request was accepted, false otherwise
   */
  virtual bool subscribe(const char* topic, uint8_t qos = 0) = 0;

  /**
   * @brief Register a callback for incoming messages on subscribed topics.
   *
   * @param cb  Callback function
   * @param ctx User context pointer passed back to the callback
   */
  virtual void setOnMessage(MessageCallback cb, void* ctx) = 0;

  /**
   * @brief Set the Last Will the broker publishes if the connection drops.
   *
   * Must be called before connect().
   *
   * @param topic    Will topic
   * @param payload  Will payload (e.g. "offline")
   * @param retained Retain flag for the will message
   * @param qos      QoS level for the will message
   */
  virtual void setWill(const char* topic, const char* payload, bool retained = true, uint8_t qos = 1) = 0;

  /**
   * @brief Check whether the MQTT client is currently connected.
   *
   * @return true if connected, false otherwise
   */
  virtual bool connected() const = 0;

  /**
   * @brief Publish an MQTT message.
   *
   * @param topic    MQTT topic (null-terminated string)
   * @param payload  Pointer to payload bytes (may be nullptr)
   * @param len      Payload length in bytes
   * @param retained Whether the message should be retained
   * @param qos      Requested QoS level (best-effort for some clients)
   *
   * @return true if the publish request was accepted, false otherwise
   *
   * @note Passing payload == nullptr publishes an empty payload, which is
   *       required for Home Assistant entity removal.
   */
  virtual bool publish(const char* topic,
                       const uint8_t* payload,
                       size_t len,
                       bool retained,
                       uint8_t qos) = 0;

  /**
   * @brief Register a callback invoked when the MQTT connection is established.
   *
   * For asynchronous transports, this is wired directly to the MQTT client's
   * onConnect handler.
   *
   * For synchronous transports, this callback is invoked when a rising-edge
   * connection transition is detected via tick().
   *
   * @param cb   Callback function
   * @param ctx  User context pointer passed back to the callback
   */
  virtual void setOnConnect(void (*cb)(void*), void* ctx) = 0;

  /**
   * @brief Set MQTT server and credentials.
   *
   * @param host MQTT host/IP
   * @param port MQTT port
   * @param user MQTT username (optional)
   * @param pass MQTT password (optional)
   */
  virtual void setServer(const char* host, uint16_t port, const char* user = nullptr, const char* pass = nullptr) = 0;

  /**
   * @brief Set MQTT server and credentials (std::string version).
   *
   * @param host MQTT host/IP
   * @param port MQTT port
   * @param user MQTT username
   * @param pass MQTT password
   */
  virtual void setServer(const std::string& host, uint16_t port, const std::string& user = "", const std::string& pass = "") = 0;

  /**
   * @brief Periodic processing hook, called from HaDiscovery::tick().
   *
   * Detects connection changes for synchronous clients, and runs automatic reconnect.
   */
  virtual void tick() {}

  /**
   * @brief Turn automatic reconnect on or off, and set its delays.
   *
   * On by default. After the connection drops, tick() tries again right away, then
   * waits @p minDelayMs, doubling after each failure up to @p maxDelayMs.
   *
   * @param enabled    Whether to reconnect automatically
   * @param minDelayMs Delay after the first failed attempt (default 1 s)
   * @param maxDelayMs Longest delay between attempts (default 60 s)
   */
  void setAutoReconnect(bool enabled, uint32_t minDelayMs = 1000, uint32_t maxDelayMs = 60000) {
    reconnect.enabled = enabled;
    reconnect.minDelayMs = minDelayMs;
    reconnect.maxDelayMs = maxDelayMs < minDelayMs ? minDelayMs : maxDelayMs;
    reconnect.currentDelayMs = minDelayMs;
  }

#ifdef ARDUINO
  // Convenience overloads for Arduino String parameters. Transports bring them into
  // scope with `using MqttTransport::setServer;` etc.

  /** @brief Overload of setServer using Arduino String (empty user/pass = none). */
  void setServer(const String& host, uint16_t port, const String& user = String(), const String& pass = String()) {
    setServer(host.c_str(), port, user.length() ? user.c_str() : nullptr, pass.length() ? pass.c_str() : nullptr);
  }

  /** @brief Overload of connect using Arduino String. */
  bool connect(const String& clientId) {
    return connect(clientId.c_str());
  }

  /** @brief Overload of subscribe using Arduino String. */
  bool subscribe(const String& topic, uint8_t qos = 0) {
    return subscribe(topic.c_str(), qos);
  }

  /** @brief Publish a String payload. */
  bool publish(const String& topic, const String& payload, bool retained = false, uint8_t qos = 0) {
    return publish(topic.c_str(), reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length(), retained, qos);
  }
#endif

  /**
   * @brief Try to reconnect on the next tick(), without waiting for the backoff delay.
   *
   * Call this when the network comes back, e.g. from the Wi-Fi "got IP" event, so the
   * transport doesn't wait for the next scheduled attempt (up to the maximum delay).
   * It also resets the delay. Safe to call from another task or an event callback;
   * it only sets a flag. Does nothing before the first connect(), while connected,
   * or when automatic reconnect is off.
   */
  void reconnectNow() {
    reconnectRequested = true;
  }

  /**
   * @brief Set the logger for this transport.
   *
   * @param logger Pointer to a JBLogger instance.
   */
  void setLogger(JBLogger* logger) {
    this->log = logger;
  }

protected:
  JBLogger* log = nullptr;
  /** @brief Reconnect timing, used by tick() in the transports. */
  ReconnectBackoff reconnect;

  /**
   * @brief Return and clear a pending reconnectNow() request. Called from tick().
   */
  bool takeReconnectRequest() {
    if (!reconnectRequested) {
      return false;
    }
    reconnectRequested = false;
    return true;
  }

private:
  volatile bool reconnectRequested = false;
};
/** @} */
