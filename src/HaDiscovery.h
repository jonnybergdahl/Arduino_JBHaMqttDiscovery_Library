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
 * @brief Entity category, which decides where Home Assistant shows an entity.
 */
enum class HaEntityCategory : uint8_t {
  None,        ///< A normal entity (default)
  Config,      ///< A setting that changes the device, shown under "Configuration". Not for sensors.
  Diagnostic   ///< Read-only device info (e.g. RSSI, uptime), shown under "Diagnostic"
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

  /**
   * @brief Where Home Assistant shows the entity (sent as `ent_cat`).
   *
   * Home Assistant doesn't allow `Config` on read-only entities (sensor, binary_sensor);
   * the library leaves it out there and logs a warning.
   */
  HaEntityCategory entity_category = HaEntityCategory::None;
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

  /**
   * @brief Seconds without a state update before Home Assistant shows the sensor as
   * unavailable (sent as `exp_aft`). 0 = never expires.
   */
  uint32_t expire_after = 0;
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

  /**
   * @brief Seconds without a state update before Home Assistant shows the sensor as
   * unavailable (sent as `exp_aft`). 0 = never expires.
   */
  uint32_t expire_after = 0;
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
 * @brief How Home Assistant shows a number entity.
 */
enum class HaNumberMode : uint8_t {
  Auto,    ///< Home Assistant decides (default)
  Box,     ///< An input box
  Slider   ///< A slider
};

/**
 * @brief Configuration for a Home Assistant MQTT Discovery number.
 *
 * A number is a value that can be set from Home Assistant. The new value arrives in the
 * onCommand() handler as text (e.g. "21.5"); publish it back with publishStateNumber().
 */
struct HaNumberConfig {
  HaEntityCommon common;

  /** @brief Optional override for command_topic (default `<base>/<node_id>/<object_id>/set`). */
  const char* command_topic_override = nullptr;

  /** @brief Lowest value (default 1, as in Home Assistant). */
  float min = 1;

  /** @brief Highest value (default 100, as in Home Assistant). */
  float max = 100;

  /** @brief Step size (default 1). */
  float step = 1;

  /** @brief How Home Assistant shows it (default Auto). */
  HaNumberMode mode = HaNumberMode::Auto;

  /** @brief Optional unit of measurement (e.g. "s"). */
  const char* unit_of_measurement = nullptr;

  /** @brief Optional device_class (e.g. "temperature"). */
  const char* device_class = nullptr;
};

/**
 * @brief Configuration for a Home Assistant MQTT Discovery select.
 *
 * A select is a choice from a fixed list of options. The chosen option arrives in the
 * onCommand() handler; publish it back with publishState().
 */
struct HaSelectConfig {
  HaEntityCommon common;

  /** @brief Optional override for command_topic (default `<base>/<node_id>/<object_id>/set`). */
  const char* command_topic_override = nullptr;

  /** @brief The options, e.g. `{"Eco", "Normal", "Turbo"}` (required, at least one). */
  std::vector<const char*> options;
};

/**
 * @brief How Home Assistant shows a text entity.
 */
enum class HaTextMode : uint8_t {
  Text,     ///< Plain text (default)
  Password  ///< Hidden input
};

/**
 * @brief Configuration for a Home Assistant MQTT Discovery text.
 *
 * A text is a string that can be set from Home Assistant. The new text arrives in the
 * onCommand() handler; publish it back with publishState().
 */
struct HaTextConfig {
  HaEntityCommon common;

  /** @brief Optional override for command_topic (default `<base>/<node_id>/<object_id>/set`). */
  const char* command_topic_override = nullptr;

  /** @brief Shortest allowed length (default 0). */
  uint16_t min_length = 0;

  /** @brief Longest allowed length (default 255, the Home Assistant maximum). */
  uint16_t max_length = 255;

  /** @brief How Home Assistant shows it (default Text). */
  HaTextMode mode = HaTextMode::Text;

  /** @brief Optional regular expression the text must match. */
  const char* pattern = nullptr;
};

/**
 * @brief What a light supports.
 */
enum class HaLightColorMode : uint8_t {
  OnOff,       ///< On and off only
  Brightness,  ///< On, off and brightness (default)
  ColorTemp,   ///< Brightness and color temperature (in kelvin)
  Rgb          ///< Brightness and RGB color
};

/**
 * @brief Configuration for a Home Assistant MQTT Discovery light.
 *
 * Uses Home Assistant's JSON schema: commands arrive in the onCommand() handler as JSON,
 * e.g. `{"state":"ON","brightness":128}`. Decode them with HaDiscovery::parseLightCommand()
 * and report the state with HaDiscovery::publishStateLight().
 */
struct HaLightConfig {
  HaEntityCommon common;

  /** @brief Optional override for command_topic (default `<base>/<node_id>/<object_id>/set`). */
  const char* command_topic_override = nullptr;

  /** @brief What the light supports (default Brightness). */
  HaLightColorMode color_mode = HaLightColorMode::Brightness;

  /** @brief Brightness for full brightness (default 255). */
  uint16_t brightness_scale = 255;

  /** @brief Warmest color temperature in kelvin, for ColorTemp (default 2000). */
  uint16_t min_kelvin = 2000;

  /** @brief Coldest color temperature in kelvin, for ColorTemp (default 6500). */
  uint16_t max_kelvin = 6500;
};

/**
 * @brief A light command from Home Assistant, decoded by HaDiscovery::parseLightCommand().
 *
 * Only the parts Home Assistant sent are set; check the has_* flags.
 */
struct HaLightCommand {
  bool has_state = false;        ///< state was sent
  bool on = false;               ///< true for "ON"
  bool has_brightness = false;   ///< brightness was sent
  uint16_t brightness = 0;       ///< 0..brightness_scale
  bool has_color_temp = false;   ///< color_temp was sent
  uint16_t color_temp_kelvin = 0;
  bool has_color = false;        ///< color was sent
  uint8_t r = 0, g = 0, b = 0;
  bool has_transition = false;   ///< transition was sent
  float transition = 0;          ///< seconds
};

/**
 * @brief The state of a light, for HaDiscovery::publishStateLight().
 */
struct HaLightState {
  bool on = false;
  /** @brief Brightness (0..brightness_scale), or -1 to leave it out. */
  int brightness = -1;
  /** @brief Color temperature in kelvin, or -1 to leave it out. */
  int color_temp_kelvin = -1;
  /** @brief RGB color, or -1 in r to leave it out. */
  int r = -1, g = 0, b = 0;
};

/**
 * @brief Configuration for a Home Assistant MQTT Discovery cover (blinds, garage door, ...).
 *
 * Open, close and stop commands arrive in the onCommand() handler as payload_open,
 * payload_close and payload_stop. If has_position is set, a position request arrives as
 * a number ("0".."100", 0 = closed). Report the state ("open", "opening", "closed",
 * "closing", "stopped") with publishState() and the position with publishStatePosition().
 */
struct HaCoverConfig {
  HaEntityCommon common;

  /** @brief Optional override for command_topic (default `<base>/<node_id>/<object_id>/set`). */
  const char* command_topic_override = nullptr;

  /** @brief Optional device_class (e.g. "blind", "garage", "shutter"). */
  const char* device_class = nullptr;

  /** @brief Payload for open (default "OPEN"). */
  const char* payload_open = nullptr;

  /** @brief Payload for close (default "CLOSE"). */
  const char* payload_close = nullptr;

  /** @brief Payload for stop (default "STOP"), or "" if the cover can't stop. */
  const char* payload_stop = nullptr;

  /**
   * @brief Whether the cover reports and accepts a position (0..100).
   *
   * Adds `<base>/<node_id>/<object_id>/position` (state) and `.../set_position` (command).
   */
  bool has_position = false;
};

/**
 * @brief Handler for commands sent by Home Assistant (switch, button, number, select, text, light, cover).
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
   * It's called when a message arrives on the command topic of an entity that takes
   * commands: switch, button, number, select, text, light or cover. After acting on it,
   * publish the new state (publishStateSwitch(), publishStateNumber(), publishStateLight(),
   * publishState() or publishStatePosition()).
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
   * @brief Add or update a number. The device payload is published from the next tick().
   *
   * New values are passed to the onCommand() handler as text.
   *
   * @return false if setDevice() wasn't called, object_id is missing, or min > max
   */
  bool publishNumberDiscovery(const HaNumberConfig& cfg);

  /**
   * @brief Add or update a select. The device payload is published from the next tick().
   *
   * The chosen option is passed to the onCommand() handler.
   *
   * @return false if setDevice() wasn't called, object_id is missing, or there are no options
   */
  bool publishSelectDiscovery(const HaSelectConfig& cfg);

  /**
   * @brief Add or update a text. The device payload is published from the next tick().
   *
   * The new text is passed to the onCommand() handler.
   *
   * @return false if setDevice() wasn't called, object_id is missing, or min_length > max_length
   */
  bool publishTextDiscovery(const HaTextConfig& cfg);

  /**
   * @brief Add or update a light (JSON schema). The device payload is published from the next tick().
   *
   * Commands are passed to the onCommand() handler as JSON; see parseLightCommand().
   */
  bool publishLightDiscovery(const HaLightConfig& cfg);

  /**
   * @brief Add or update a cover. The device payload is published from the next tick().
   *
   * Open/close/stop and position commands are passed to the onCommand() handler.
   */
  bool publishCoverDiscovery(const HaCoverConfig& cfg);

  /**
   * @brief Remove one entity from the device.
   *
   * From the next tick(), the device payload is published twice: once with the entity
   * as a removal stub (`{"p": "<component>"}`), then without it. Commands for a removed
   * entity are no longer passed to the command handler, and its stored state
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

  /**
   * @brief Publish a number state, e.g. 21.5 with decimals = 1 -> "21.5".
   *
   * @param object_id Entity object_id
   * @param value     The value
   * @param decimals  Digits after the decimal point (default 2)
   */
  bool publishStateNumber(const char* object_id, double value, uint8_t decimals = 2, bool retained = false, uint8_t qos = 0);

  /**
   * @brief Publish a light state as JSON, e.g. `{"state":"ON","brightness":128,"color_mode":"brightness"}`.
   *
   * The color_mode is set from what's included: color, then color temperature, then
   * brightness, otherwise onoff.
   */
  bool publishStateLight(const char* object_id, const HaLightState& state, bool retained = false, uint8_t qos = 0);

  /**
   * @brief Publish a cover position (0..100) to `<base>/<node_id>/<object_id>/position`.
   */
  bool publishStatePosition(const char* object_id, int position, bool retained = false, uint8_t qos = 0);

  /**
   * @brief Decode a light command from the onCommand() handler.
   *
   * @param payload The JSON payload, e.g. `{"state":"ON","brightness":128}`
   * @param out     The decoded command; only the parts that were sent are set
   * @return false if the payload isn't valid JSON
   */
  static bool parseLightCommand(const char* payload, HaLightCommand& out);

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
  void setCommandTopics(const char* object_id, const std::vector<std::string>& topics);
  std::string commandTopicFor(const char* override_topic, const char* object_id) const;
  bool publishToTopic(const std::string& topic, const char* payload, bool retained, uint8_t qos);
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
  std::string buildNumberConfigJson(const HaNumberConfig& cfg) const;
  std::string buildSelectConfigJson(const HaSelectConfig& cfg) const;
  std::string buildTextConfigJson(const HaTextConfig& cfg) const;
  std::string buildLightConfigJson(const HaLightConfig& cfg) const;
  std::string buildCoverConfigJson(const HaCoverConfig& cfg) const;
  bool addEntity(const char* component, const HaEntityCommon& common, std::string json);

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
