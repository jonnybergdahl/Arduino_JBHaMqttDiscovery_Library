#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>

#if __has_include(<jblogger.h>)
#include <jblogger.h>
#else
/**
 * @brief Log level enumeration.
 */
enum LogLevel {
  LOG_LEVEL_NONE = 0,    /**< No logging */
  LOG_LEVEL_ERROR,       /**< Critical errors */
  LOG_LEVEL_WARNING,     /**< Warnings */
  LOG_LEVEL_INFO,        /**< Informational messages */
  LOG_LEVEL_DEBUG,       /**< Debug information */
  LOG_LEVEL_TRACE        /**< High-frequency trace data */
};

/**
 * @brief Minimal logger interface used when jblogger.h is missing.
 */
class JBLogger {
public:
  /** @brief Constructor. @param moduleName Name of the module. */
  JBLogger(const char* moduleName) {}
  /** @brief Log a debug message. @param format Format string. */
  virtual void debug(const char* format, ...) {}
  /** @brief Log an info message. @param format Format string. */
  virtual void info(const char* format, ...) {}
  /** @brief Log a warning message. @param format Format string. */
  virtual void warn(const char* format, ...) {}
  /** @brief Log an error message. @param format Format string. */
  virtual void error(const char* format, ...) {}
  /** @brief Set the log level. @param level Minimum log level to log. */
  virtual void setLogLevel(LogLevel level) {}
};
#endif

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
   * @brief Periodic processing hook (optional).
   *
   * This is required only for synchronous MQTT clients that do not provide
   * a native connection callback.
   *
   * The host firmware should call this regularly (e.g. from loop()) if using
   * such a transport.
   */
  virtual void tick() {}

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
};
/** @} */
