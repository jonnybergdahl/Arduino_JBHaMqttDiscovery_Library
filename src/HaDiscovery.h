#pragma once
#include <stddef.h>
#include <stdint.h>
#include <functional>
#if defined(ESP32) || !defined(ARDUINO)
#include <mutex>
#endif
#include <string>
#include <vector>
#include <ArduinoJson.h>
#include "transport/MqttTransport.h"

/** @brief Library name, sent as the default origin name. */
#define JBHAMQTTDISCOVERY_NAME "JBHaMqttDiscovery"
/** @brief Library version, sent as the default origin version. Keep in sync with library.properties. */
#define JBHAMQTTDISCOVERY_VERSION "1.0.0"
/** @brief Library URL, sent as the default origin support URL. */
#define JBHAMQTTDISCOVERY_URL "https://github.com/jonnybergdahl/Arduino_JBHaMqttDiscovery"

/**
 * @defgroup hadiscovery Home Assistant MQTT Discovery
 * @brief Publish Home Assistant MQTT Discovery config payloads via an abstract MQTT transport.
 *
 * This module builds a Home Assistant MQTT device discovery payload and publishes it
 * using a transport adapter (e.g. PubSubClient or AsyncMqttClient).
 *
 * Key behaviors:
 * - Publishes one retained device discovery payload with all entities as components,
 *   to `<discovery_prefix>/device/<node_id>/config`
 * - Publishes availability ("online"/"offline") and sets "offline" as the MQTT Last Will
 * - Subscribes to switch and button command topics and passes commands to a handler
 * - Remembers the entities and last states, and sends them again on every connect
 *   and when Home Assistant comes back online (birth message on `<discovery_prefix>/status`)
 * - Provides default topic conventions with per-entity overrides
 * - Removes single entities or the whole device
 *
 * @{
 */

/**
 * @brief Home Assistant device information for the "dev" block in MQTT Discovery payloads.
 */
struct HaDeviceInfo {
  /**
   * @brief Stable node identifier used in topics and unique_id (e.g. "esp32_kitchen_01").
   *
   * Only letters, digits, `_` and `-` are allowed, since it's part of the discovery topic.
   */
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

  /**
   * @brief Optional URL for the device's web interface, e.g. "http://192.168.1.50/".
   *
   * Home Assistant shows it as a "Visit" link on the device page. Sent as `dev.cu`
   * (`configuration_url`). Must be an http(s) URL, or a `homeassistant://` path.
   */
  const char* configuration_url = nullptr;
};

/**
 * @brief Common options shared by multiple entity types.
 */
struct HaEntityCommon {
  /** @brief Stable per-entity object_id (e.g. "temperature", "relay1"). */
  const char* object_id = nullptr;

  /**
   * @brief Entity name shown in Home Assistant, without the device name.
   *
   * Home Assistant puts the device name in front, so "Temperature" on device
   * "Office Node" shows as "Office Node Temperature". If nullptr, object_id is used.
   */
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
   *
   * @note The MQTT Last Will only covers the default availability topic.
   */
  const char* availability_topic_override = nullptr;

  /**
   * @brief Optional entity ID Home Assistant should use, without the domain.
   *
   * Sent as `def_ent_id` with the domain added, so "office_temperature" on a sensor
   * becomes `sensor.office_temperature`. Use lowercase letters, digits and underscores;
   * uppercase letters are lowercased and other characters replaced with `_`.
   * Home Assistant only uses it when it first creates the entity. If nullptr, Home
   * Assistant makes one up from the device and entity names.
   */
  const char* entity_id = nullptr;

  /**
   * @brief Mark this entity as the device's main feature.
   *
   * Sends `"name": null`, so the entity is shown with just the device name.
   * The name field is ignored when this is true.
   */
  bool use_device_name = false;
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
 * @brief Handler for commands sent by Home Assistant to a switch or button.
 *
 * @param object_id The entity's object_id
 * @param payload   The command payload, null-terminated (e.g. "ON", "OFF", "PRESS")
 */
using HaCommandCallback = std::function<void(const char* object_id, const char* payload)>;

/**
 * @brief Home Assistant MQTT device discovery publisher (transport-agnostic).
 *
 * Typical usage:
 * - Construct with a MqttTransport implementation
 * - Provide device info via setDevice(), before connecting (this sets the Last Will)
 * - Register a command handler with onCommand() if you have switches or buttons
 * - Connect through the transport
 * - Add entities with the publish*Discovery() methods
 * - Call tick() from loop(), and publish states as needed
 *
 * The publish*Discovery() methods add or update an entity. The device payload with all
 * entities is published from the next tick(), so adding several entities in setup()
 * sends one message. States published before that are sent right after it.
 *
 * Everything is kept in RAM so it can be sent again after a reconnect or a Home
 * Assistant restart: about 150–250 bytes per entity. The device payload (about 400
 * bytes plus that per entity) is built from it only while it's being published.
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
   * @brief Set the device information used in the `dev` object of the discovery payload.
   *
   * Call this before adding entities. The strings are copied, so the pointers in
   * @p dev only need to be valid during this call.
   *
   * This also sets the MQTT Last Will to "offline" on the default availability topic,
   * so call it before connecting.
   *
   * @param dev Device info struct
   */
  void setDevice(const HaDeviceInfo& dev);

  /**
   * @brief Set the handler for commands from Home Assistant.
   *
   * It's called when a message arrives on the command topic of a switch or button
   * added with publishSwitchDiscovery() or publishButtonDiscovery().
   * For a switch, publish the new state with publishStateSwitch() after acting on it.
   *
   * Commands are queued when they arrive and the handler is called from tick(), so it
   * always runs in loop(), also with AsyncMqttClient.
   *
   * @param cb Command handler
   */
  void onCommand(HaCommandCallback cb);

  /**
   * @brief Set the origin (`o`) sent in the discovery payload.
   *
   * Home Assistant logs it when entities are discovered, and shows it in the
   * MQTT device info. It defaults to this library's name, version and URL.
   * Set it to your firmware's name, version and URL instead if you like.
   *
   * @param name       Name of the software publishing discovery (required)
   * @param sw_version Software version (optional)
   * @param url        Support URL (optional)
   */
  void setOrigin(const char* name, const char* sw_version = nullptr, const char* url = nullptr);

  /**
   * @brief Periodic processing hook. Call this from loop().
   *
   * It drives the transport (connection changes and reconnect), handles queued
   * commands and birth messages, and does all publishing:
   * - after a connect: subscribes, then publishes the device payload, "online" and the stored states
   * - after entities were added or removed: publishes the device payload and the stored states
   * - after a Home Assistant birth message and a random delay: publishes everything again
   */
  void tick();

  /**
   * @brief Set the maximum random delay before re-publishing after Home Assistant comes online.
   *
   * Home Assistant recommends a random delay so many devices don't publish at once.
   *
   * @param maxDelayMs Maximum delay in milliseconds (default 5000, 0 = next tick())
   */
  void setRepublishDelay(uint32_t maxDelayMs);

  /**
   * @brief Publish the device payload, availability "online" and the last states now.
   *
   * This happens automatically on every connect, after entity changes and after a
   * Home Assistant birth message. Does nothing while disconnected.
   */
  void republishAll();

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
   * @brief Add or update a sensor. The device payload is published from the next tick().
   *
   * @param cfg Sensor configuration
   * @return false if setDevice() wasn't called, object_id is missing, or the config couldn't be built
   */
  bool publishSensorDiscovery(const HaSensorConfig& cfg);

  /**
   * @brief Add or update a switch. The device payload is published from the next tick().
   *
   * Commands for it are passed to the onCommand() handler.
   *
   * @param cfg Switch configuration
   * @return false if setDevice() wasn't called, object_id is missing, or the config couldn't be built
   */
  bool publishSwitchDiscovery(const HaSwitchConfig& cfg);

  /**
   * @brief Add or update a binary_sensor. The device payload is published from the next tick().
   *
   * @param cfg Binary sensor configuration
   * @return false if setDevice() wasn't called, object_id is missing, or the config couldn't be built
   */
  bool publishBinarySensorDiscovery(const HaBinarySensorConfig& cfg);

  /**
   * @brief Add or update a button. The device payload is published from the next tick().
   *
   * Presses are passed to the onCommand() handler.
   *
   * @param cfg Button configuration
   * @return false if setDevice() wasn't called, object_id is missing, or the config couldn't be built
   */
  bool publishButtonDiscovery(const HaButtonConfig& cfg);

  /**
   * @brief Remove one entity from the device.
   *
   * From the next tick(), the device payload is published twice: once with the entity
   * as a removal stub (`{"p": "<component>"}`), then without it. Commands for a removed
   * switch or button are no longer passed to the command handler, and its stored state
   * is dropped. Removing the last entity also clears the device discovery topic.
   *
   * @param component Component name (e.g. "sensor", "switch")
   * @param object_id Entity object_id
   * @return true if the entity was found
   */
  bool removeEntity(const char* component, const char* object_id);

  /**
   * @brief Remove the device and all its entities from Home Assistant.
   *
   * Publishes an empty retained payload to the device discovery topic right away
   * (if connected), and forgets all entities and states.
   *
   * @return true if the publish was accepted by the transport
   */
  bool removeDevice();

  /**
   * @brief Publish an entity state payload using default state topic for object_id.
   *
   * The last payload per entity is stored and sent again on connect and after a
   * Home Assistant restart. While disconnected, or while the device payload is waiting
   * to be published, it is only stored, and this returns true.
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

  // Convenience overloads for std::string parameters

  /** @brief Overload of removeEntity using std::string. */
  inline bool removeEntity(const std::string& component, const std::string& object_id) {
    return removeEntity(component.c_str(), object_id.c_str());
  }
  /** @brief Overload of removeEntity using std::string for object_id. */
  inline bool removeEntity(const char* component, const std::string& object_id) {
    return removeEntity(component, object_id.c_str());
  }
  /** @brief Overload of removeEntity using std::string for component. */
  inline bool removeEntity(const std::string& component, const char* object_id) {
    return removeEntity(component.c_str(), object_id);
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

private:
  /** @brief Lock for the incoming message queue (shared with the AsyncTCP task on ESP32). */
#if defined(ESP32) || !defined(ARDUINO)
  using QueueMutex = std::mutex;
#else
  // ESP8266 network callbacks never interrupt loop(), so no lock is needed
  struct QueueMutex {
    void lock() {}
    void unlock() {}
  };
#endif

  /** @brief A component of the device payload. */
  struct Component {
    std::string component;  // platform, e.g. "sensor"
    std::string object_id;  // key in "cmps"
    std::string json;       // serialized component config
  };

  /** @brief A command topic and the object_id it belongs to. */
  struct CommandRoute {
    std::string topic;
    std::string object_id;
  };

  /** @brief The last state published on a topic, kept for re-publishing. */
  struct StateEntry {
    std::string topic;
    std::string payload;
    bool retained;
    uint8_t qos;
  };

  /** @brief A message received from the transport, handled in tick(). */
  struct IncomingMessage {
    std::string topic;
    std::string payload;
  };

  /** @brief Origin information sent as `o`. */
  struct OriginData {
    std::string name = JBHAMQTTDISCOVERY_NAME;
    std::string sw_version = JBHAMQTTDISCOVERY_VERSION;
    std::string url = JBHAMQTTDISCOVERY_URL;
  };

  /** @brief Owned copies of the HaDeviceInfo strings (empty means not set). */
  struct DeviceData {
    std::string node_id;
    std::string name;
    std::string manufacturer;
    std::string model;
    std::string sw_version;
    std::string identifiers;
    std::string configuration_url;
  };

  static void onTransportConnectThunk(void* ctx);
  void onTransportConnect();
  static void onTransportMessageThunk(void* ctx, const char* topic, const uint8_t* payload, size_t len);
  void onTransportMessage(const char* topic, const uint8_t* payload, size_t len);
  void processIncoming();
  void handleMessage(const IncomingMessage& msg);

  void registerCommandTopic(const std::string& topic, const char* object_id);
  void unregisterCommandTopics(const char* object_id);
  void subscribeAll();

  std::string buildDeviceTopic() const;
  std::string buildDefaultStateTopic(const char* object_id) const;
  std::string buildDefaultCommandTopic(const char* object_id) const;
  std::string buildDefaultAvailabilityTopic() const;
  std::string buildBirthTopic() const;

  bool addComponent(const char* component, const char* object_id, std::string json);
  std::string buildDevicePayload(bool withRemovalStubs) const;
  void publishDevicePayload();
  bool publishRetained(const std::string& topic, const std::string& payload, uint8_t qos);
  bool canPublishStateNow() const;

  // Each returns the serialized component config, or an empty string on failure
  std::string buildSensorConfigJson(const HaSensorConfig& cfg) const;
  std::string buildSwitchConfigJson(const HaSwitchConfig& cfg) const;
  std::string buildBinarySensorConfigJson(const HaBinarySensorConfig& cfg) const;
  std::string buildButtonConfigJson(const HaButtonConfig& cfg) const;

  void addCommonFields(JsonDocument& doc, const char* component, const HaEntityCommon& common) const;
  std::string stateTopicFor(const HaEntityCommon& common) const;
  std::string sanitizeEntityId(const char* entity_id) const;
  static std::string serializeDoc(const JsonDocument& doc);

  void addDeviceObject(JsonDocument& doc) const;
  bool hasDevice() const { return !_device.node_id.empty(); }

  MqttTransport& _transport;
  std::string _discoveryPrefix;
  std::string _baseTopicPrefix;
  mutable JBLogger _log;  // logging from const methods doesn't change state
  DeviceData _device;
  OriginData _origin;

  std::vector<Component> _components;
  std::vector<Component> _removed;   // removed since the last device payload; json unused
  bool _clearDeviceTopic = false;    // last entity removed: clear the device topic after the stubs
  bool _configDirty = false;         // device payload changed and not published yet
  std::vector<CommandRoute> _commands;
  std::vector<StateEntry> _states;
  HaCommandCallback _onCommand;

  // Filled from the network task with AsyncMqttClient, emptied in tick()
  QueueMutex _incomingLock;
  std::vector<IncomingMessage> _incoming;
  static constexpr size_t MAX_INCOMING = 16;

  // Set from the network task with AsyncMqttClient, read in tick()
  volatile bool _connectPending = false;

  bool _republishPending = false;
  uint32_t _republishAt = 0;
  uint32_t _republishDelayMaxMs = 5000;
};

/** @} */
