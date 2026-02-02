#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include "transport/MqttTransport.h"

/**
 * @defgroup hadiscovery Home Assistant MQTT Discovery
 * @brief Publish Home Assistant MQTT Discovery config payloads via an abstract MQTT transport.
 *
 * This module generates Home Assistant MQTT Discovery topics and JSON payloads and publishes them
 * using a transport adapter (e.g. PubSubClient or AsyncMqttClient).
 *
 * Key behaviors:
 * - Publishes retained Discovery config messages
 * - Publishes availability ("online"/"offline")
 * - Provides default topic conventions with per-entity overrides
 * - Supports removal of entities by publishing an empty retained config payload
 *
 * @{
 */

/**
 * @brief Home Assistant device information for the "dev" block in MQTT Discovery payloads.
 */
struct HaDeviceInfo {
  /** @brief Stable node identifier used in topics and unique_id (e.g. "esp32_kitchen_01"). */
  const char* node_id = nullptr;

  /** @brief Device name shown in Home Assistant device registry. */
  const char* name = nullptr;

  /** @brief Manufacturer name. */
  const char* manufacturer = nullptr;

  /** @brief Model name. */
  const char* model = nullptr;

  /** @brief Software version string. */
  const char* sw_version = nullptr;

  /**
   * @brief Stable device identifier string (e.g. MAC address).
   *
   * This maps to the Home Assistant `dev.ids` field.
   * It should be stable across reboots/flashes to avoid device duplication.
   */
  const char* identifiers = nullptr;
};

/**
 * @brief Common options shared by multiple entity types.
 */
struct HaEntityCommon {
  /** @brief Stable per-entity object_id (e.g. "temperature", "relay1"). */
  const char* object_id = nullptr;

  /** @brief Human-friendly entity name shown in Home Assistant. */
  const char* name = nullptr;

  /** @brief Optional icon (e.g. "mdi:thermometer"). */
  const char* icon = nullptr;

  /**
   * @brief Optional override for state_topic.
   *
   * If nullptr, the default topic scheme is used:
   * `<baseTopicPrefix>/<node_id>/<object_id>/state`.
   */
  const char* state_topic_override = nullptr;

  /**
   * @brief Optional override for availability topic.
   *
   * If nullptr, the default is used:
   * `<baseTopicPrefix>/<node_id>/status`.
   */
  const char* availability_topic_override = nullptr;
};

/**
 * @brief Configuration for a Home Assistant MQTT Discovery sensor.
 */
struct HaSensorConfig {
  HaEntityCommon common;

  /** @brief Optional unit of measurement (e.g. "°C"). */
  const char* unit_of_measurement = nullptr;

  /** @brief Optional device_class (e.g. "temperature"). */
  const char* device_class = nullptr;

  /** @brief Optional state_class (e.g. "measurement"). */
  const char* state_class = nullptr;
};

/**
 * @brief Configuration for a Home Assistant MQTT Discovery switch.
 */
struct HaSwitchConfig {
  HaEntityCommon common;

  /**
   * @brief Optional override for command_topic.
   *
   * If nullptr, the default topic scheme is used:
   * `<baseTopicPrefix>/<node_id>/<object_id>/set`.
   */
  const char* command_topic_override = nullptr;

  /** @brief Payload representing ON state/command (default "ON" if nullptr). */
  const char* payload_on = nullptr;

  /** @brief Payload representing OFF state/command (default "OFF" if nullptr). */
  const char* payload_off = nullptr;
};

/**
 * @brief Configuration for a Home Assistant MQTT Discovery binary_sensor.
 */
struct HaBinarySensorConfig {
  HaEntityCommon common;

  /** @brief Optional device_class (e.g. "motion", "door", "presence"). */
  const char* device_class = nullptr;

  /** @brief Optional payload representing ON state (default "ON" if nullptr). */
  const char* payload_on = nullptr;

  /** @brief Optional payload representing OFF state (default "OFF" if nullptr). */
  const char* payload_off = nullptr;
};

/**
 * @brief Configuration for a Home Assistant MQTT Discovery button.
 *
 * A button is a stateless entity that triggers an action when a payload is published
 * to its command topic.
 */
struct HaButtonConfig {
  HaEntityCommon common;

  /**
   * @brief Optional override for command_topic.
   *
   * If nullptr, the default topic scheme is used:
   * `<baseTopicPrefix>/<node_id>/<object_id>/set`.
   */
  const char* command_topic_override = nullptr;

  /**
   * @brief Optional payload to trigger the button (default "PRESS" if nullptr).
   */
  const char* payload_press = nullptr;
};

/**
 * @brief Home Assistant MQTT Discovery publisher (transport-agnostic).
 *
 * Typical usage:
 * - Construct with a MqttTransport implementation
 * - Provide device info via setDevice()
 * - Publish discovery configs (retained)
 * - Publish states as needed
 *
 * For async transports, onConnect is invoked automatically.
 * For sync transports, call tick() periodically to detect reconnection transitions.
 */
class HaDiscovery {
public:
  /**
   * @brief Construct a Home Assistant MQTT Discovery publisher.
   *
   * @param transport         MQTT transport adapter
   * @param discovery_prefix  Home Assistant discovery prefix (default "homeassistant")
   * @param base_topic_prefix Base topic prefix for device topics (default "devices")
   * @param log_level         Log level for the internal logger (default LOG_LEVEL_INFO)
   */
  HaDiscovery(MqttTransport& transport,
              const char* discovery_prefix = "homeassistant",
              const char* base_topic_prefix = "devices",
              LogLevel log_level = LogLevel::LOG_LEVEL_INFO);

  /**
   * @brief Set the minimum log level for the internal logger.
   *
   * @param level The minimum log level to be logged.
   */
  void setLogLevel(LogLevel level);

  /**
   * @brief Set the device information used in `dev` object of Discovery payloads.
   *
   * This must be called before publishing discovery configs if you want correct device registry behavior.
   *
   * @param dev Device info struct (pointers must remain valid)
   */
  void setDevice(const HaDeviceInfo& dev);

  /**
   * @brief Periodic processing hook.
   *
   * For synchronous MQTT clients (e.g. PubSubClient), call this from loop().
   * For asynchronous clients, this is typically not required but harmless.
   */
  void tick();

  /**
   * @brief Publish "online" availability payload to the availability topic (retained by default).
   *
   * @param retained Retain flag (recommended true)
   * @param qos      QoS level (recommended 1 for availability if supported)
   */
  void publishAvailabilityOnline(bool retained = true, uint8_t qos = 1);

  /**
   * @brief Publish "offline" availability payload to the availability topic (retained by default).
   *
   * @param retained Retain flag (recommended true)
   * @param qos      QoS level (recommended 1 for availability if supported)
   */
  void publishAvailabilityOffline(bool retained = true, uint8_t qos = 1);

  /**
   * @brief Publish a sensor Discovery config (retained by default).
   *
   * @param cfg      Sensor configuration
   * @param retained Retain flag (recommended true)
   * @param qos      QoS level (recommended 1 for discovery if supported)
   * @return true if publish was accepted by transport, false otherwise
   */
  bool publishSensorDiscovery(const HaSensorConfig& cfg, bool retained = true, uint8_t qos = 1);

  /**
   * @brief Publish a switch Discovery config (retained by default).
   *
   * @param cfg      Switch configuration
   * @param retained Retain flag (recommended true)
   * @param qos      QoS level (recommended 1 for discovery if supported)
   * @return true if publish was accepted by transport, false otherwise
   */
  bool publishSwitchDiscovery(const HaSwitchConfig& cfg, bool retained = true, uint8_t qos = 1);

  /**
   * @brief Publish a binary_sensor Discovery config (retained by default).
   *
   * @param cfg      Binary sensor configuration
   * @param retained Retain flag (recommended true)
   * @param qos      QoS level (recommended 1 for discovery if supported)
   * @return true if publish was accepted by transport, false otherwise
   */
  bool publishBinarySensorDiscovery(const HaBinarySensorConfig& cfg, bool retained = true, uint8_t qos = 1);

  /**
   * @brief Publish a button Discovery config (retained by default).
   *
   * @param cfg      Button configuration
   * @param retained Retain flag (recommended true)
   * @param qos      QoS level (recommended 1 for discovery if supported)
   * @return true if publish was accepted by transport, false otherwise
   */
  bool publishButtonDiscovery(const HaButtonConfig& cfg, bool retained = true, uint8_t qos = 1);


  /**
   * @brief Remove an entity from Home Assistant by clearing its retained config topic.
   *
   * Home Assistant removes the entity when the Discovery config topic is published
   * with an empty payload and retain=true.
   *
   * @param component Component name (e.g. "sensor", "switch")
   * @param object_id Entity object_id used in the config topic
   * @param qos       QoS level (recommended 1 if supported)
   * @return true if publish was accepted by transport, false otherwise
   */
  bool removeEntity(const char* component, const char* object_id, uint8_t qos = 1);

  /**
   * @brief Publish an entity state payload using default state topic for object_id.
   *
   * @param object_id Entity object_id
   * @param payload   Null-terminated payload string
   * @param retained  Retain flag (usually false for state)
   * @param qos       QoS level (usually 0 for state)
   * @return true if publish was accepted by transport, false otherwise
   */
  bool publishState(const char* object_id, const char* payload, bool retained = false, uint8_t qos = 0);

  /**
   * @brief Publish a switch state ("ON"/"OFF") using default state topic.
   *
   * @param object_id Entity object_id
   * @param on        true -> "ON", false -> "OFF"
   * @param retained  Retain flag (usually false)
   * @param qos       QoS level (usually 0)
   * @return true if publish was accepted by transport, false otherwise
   */
  bool publishStateSwitch(const char* object_id, bool on, bool retained = false, uint8_t qos = 0);

  /**
   * @brief Publish a button "press" command to the default command topic.
   *
   * @param object_id Entity object_id
   * @param payload   Payload string to publish (if nullptr uses default "PRESS")
   * @param retained  Retain flag (usually false)
   * @param qos       QoS level (usually 0 or 1)
   * @return true if publish was accepted by transport, false otherwise
   */
  bool pressButton(const char* object_id, const char* payload = nullptr, bool retained = false, uint8_t qos = 0);

  // Convenience overloads for std::string parameters

  /** @brief Overload of removeEntity using std::string. */
  inline bool removeEntity(const std::string& component, const std::string& object_id, uint8_t qos = 1) {
    return removeEntity(component.c_str(), object_id.c_str(), qos);
  }
  /** @brief Overload of removeEntity using std::string for object_id. */
  inline bool removeEntity(const char* component, const std::string& object_id, uint8_t qos = 1) {
    return removeEntity(component, object_id.c_str(), qos);
  }
  /** @brief Overload of removeEntity using std::string for component. */
  inline bool removeEntity(const std::string& component, const char* object_id, uint8_t qos = 1) {
    return removeEntity(component.c_str(), object_id, qos);
  }

  /** @brief Overload of publishState using std::string. */
  inline bool publishState(const std::string& object_id, const std::string& payload, bool retained = false, uint8_t qos = 0) {
    return publishState(object_id.c_str(), payload.c_str(), retained, qos);
  }
  /** @brief Overload of publishState using std::string for object_id. */
  inline bool publishState(const std::string& object_id, const char* payload, bool retained = false, uint8_t qos = 0) {
    return publishState(object_id.c_str(), payload, retained, qos);
  }
  /** @brief Overload of publishState using std::string for payload. */
  inline bool publishState(const char* object_id, const std::string& payload, bool retained = false, uint8_t qos = 0) {
    return publishState(object_id, payload.c_str(), retained, qos);
  }

  /** @brief Overload of publishStateSwitch using std::string for object_id. */
  inline bool publishStateSwitch(const std::string& object_id, bool on, bool retained = false, uint8_t qos = 0) {
    return publishStateSwitch(object_id.c_str(), on, retained, qos);
  }

  /** @brief Overload of pressButton using std::string. */
  inline bool pressButton(const std::string& object_id, const std::string& payload, bool retained = false, uint8_t qos = 0) {
    return pressButton(object_id.c_str(), payload.c_str(), retained, qos);
  }
  /** @brief Overload of pressButton using std::string for object_id. */
  inline bool pressButton(const std::string& object_id, const char* payload, bool retained = false, uint8_t qos = 0) {
    return pressButton(object_id.c_str(), payload, retained, qos);
  }
  /** @brief Overload of pressButton using std::string for object_id (no payload). */
  inline bool pressButton(const std::string& object_id, bool retained = false, uint8_t qos = 0) {
    return pressButton(object_id.c_str(), nullptr, retained, qos);
  }

private:
  static void onTransportConnectThunk(void* ctx);
  void onTransportConnect();

  std::string buildConfigTopic(const char* component, const char* object_id) const;
  std::string buildDefaultStateTopic(const char* object_id) const;
  std::string buildDefaultCommandTopic(const char* object_id) const;
  std::string buildDefaultAvailabilityTopic() const;

  bool publishConfigJson(const char* topic, const char* json, bool retained, uint8_t qos);

  bool buildSensorConfigJson(char* out, size_t outLen, const HaSensorConfig& cfg) const;
  bool buildSwitchConfigJson(char* out, size_t outLen, const HaSwitchConfig& cfg) const;
  bool buildBinarySensorConfigJson(char* out, size_t outLen, const HaBinarySensorConfig& cfg) const;
  bool buildButtonConfigJson(char* out, size_t outLen, const HaButtonConfig& cfg) const;


private:
  MqttTransport& _transport;
  std::string _discoveryPrefix;
  std::string _baseTopicPrefix;
  JBLogger* _log;
  HaDeviceInfo _device{};

  static constexpr size_t JSON_BUF = 768;
};

/** @} */
