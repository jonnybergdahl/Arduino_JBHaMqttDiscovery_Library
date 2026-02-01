#pragma once
#include <stddef.h>
#include <stdint.h>

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
 * - reporting connection state
 * - publishing MQTT messages
 * - notifying when a connection is (re)established
 *
 * It does NOT:
 * - manage Wi-Fi
 * - manage MQTT configuration
 * - perform polling (except via tick() for sync clients)
 */
class MqttTransport {
public:
  virtual ~MqttTransport() = default;

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
   * @brief Periodic processing hook (optional).
   *
   * This is required only for synchronous MQTT clients that do not provide
   * a native connection callback.
   *
   * The host firmware should call this regularly (e.g. from loop()) if using
   * such a transport.
   */
  virtual void tick() {}
};
/** @} */
