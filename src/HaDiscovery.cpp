#include "HaDiscovery.h"
#include <stdio.h>
#include <string.h>
#include <ArduinoJson.h>

#ifdef ARDUINO
#include <Arduino.h>
static uint32_t nowMs() { return millis(); }
static uint32_t randomMs(uint32_t max) { return max ? static_cast<uint32_t>(random(static_cast<long>(max) + 1)) : 0; }
#else
#include <chrono>
#include <cstdlib>
static uint32_t nowMs() {
  using namespace std::chrono;
  return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
static uint32_t randomMs(uint32_t max) { return max ? static_cast<uint32_t>(rand()) % (max + 1) : 0; }
#endif

// The logger header itself is included through transport/MqttTransport.h
#ifndef NO_JB_LOGGER
#define HAS_LOG
#endif

static const char* kAvailOnline = "online";
static const char* kAvailOffline = "offline";
static const char* kOn = "ON";
static const char* kOff = "OFF";
static const char* kPress = "PRESS";

HaDiscovery::HaDiscovery(MqttTransport& transport,
                         const char* discovery_prefix,
                         const char* base_topic_prefix,
                         LogLevel log_level)
  : _transport(transport),
    _discoveryPrefix(discovery_prefix ? discovery_prefix : "homeassistant"),
    _baseTopicPrefix(base_topic_prefix ? base_topic_prefix : "devices"),
    _log("HaDiscovery", log_level) {
  _transport.setLogger(&_log);
  _transport.setOnConnect(&HaDiscovery::onTransportConnectThunk, this);
  _transport.setOnMessage(&HaDiscovery::onTransportMessageThunk, this);
}

void HaDiscovery::setLogLevel(LogLevel level) {
  _log.setLogLevel(level);
}

void HaDiscovery::setDevice(const HaDeviceInfo& dev) {
  auto copy = [](const char* v) { return v ? std::string(v) : std::string(); };
  _device.node_id = copy(dev.node_id);
  _device.name = copy(dev.name);
  _device.manufacturer = copy(dev.manufacturer);
  _device.model = copy(dev.model);
  _device.sw_version = copy(dev.sw_version);
  _device.identifiers = copy(dev.identifiers);

  if (hasDevice()) {
    // The broker publishes this if the device drops off without disconnecting
    _transport.setWill(buildDefaultAvailabilityTopic().c_str(), kAvailOffline, true, 1);
  }
}

void HaDiscovery::setOrigin(const char* name, const char* sw_version, const char* url) {
  _origin.name = name ? name : JBHAMQTTDISCOVERY_NAME;
  _origin.sw_version = sw_version ? sw_version : "";
  _origin.url = url ? url : "";
}

void HaDiscovery::onCommand(HaCommandCallback cb) {
  _onCommand = cb;
}

void HaDiscovery::setRepublishDelay(uint32_t maxDelayMs) {
  _republishDelayMaxMs = maxDelayMs;
}

bool HaDiscovery::canPublishNow() const {
  // While a connect is pending, tick() is about to send everything anyway
  return _transport.connected() && !_connectPending;
}

void HaDiscovery::tick() {
  _transport.tick();

  if (_connectPending) {
    if (!_transport.connected()) {
      return;
    }
    _connectPending = false;
    _republishPending = false;

    // Subscriptions don't survive a clean-session reconnect
    _transport.subscribe(buildBirthTopic().c_str(), 1);
    for (const CommandRoute& route : _commands) {
      _transport.subscribe(route.topic.c_str(), 1);
    }

    // A reconnect may follow a broker restart that lost retained messages,
    // and anything published while disconnected was only stored
    republishAll();
    return;
  }

  if (_republishPending && static_cast<int32_t>(nowMs() - _republishAt) >= 0) {
    _republishPending = false;
    if (_transport.connected()) {
      republishAll();
    }
  }
}

void HaDiscovery::republishAll() {
  if (!_transport.connected()) {
    return;
  }
#ifdef HAS_LOG
  _log.info("Publishing %u discovery configs and %u states",
            (unsigned)_entities.size(), (unsigned)_states.size());
#endif
  // Configs first, so Home Assistant has the entities before their state arrives
  for (const EntityConfig& e : _entities) {
    publishConfigJson(e.topic.c_str(), e.json.c_str(), e.retained, e.qos);
  }
  publishAvailabilityOnline(true, 1);
  for (const StateEntry& st : _states) {
    _transport.publish(st.topic.c_str(),
                       reinterpret_cast<const uint8_t*>(st.payload.data()),
                       st.payload.size(),
                       st.retained,
                       st.qos);
  }
}

void HaDiscovery::onTransportConnectThunk(void* ctx) {
  static_cast<HaDiscovery*>(ctx)->onTransportConnect();
}

void HaDiscovery::onTransportConnect() {
#ifdef HAS_LOG
  _log.info("MQTT Transport connected");
#endif
  // With AsyncMqttClient this runs in the network task, so only set a flag.
  // tick() subscribes and publishes from loop().
  _connectPending = true;
}

void HaDiscovery::onTransportMessageThunk(void* ctx, const char* topic, const uint8_t* payload, size_t len) {
  static_cast<HaDiscovery*>(ctx)->onTransportMessage(topic, payload, len);
}

void HaDiscovery::onTransportMessage(const char* topic, const uint8_t* payload, size_t len) {
  if (!topic) {
    return;
  }

  if (buildBirthTopic() == topic) {
    // Home Assistant (re)started and has forgotten non-retained state.
    // Wait a random time so many devices don't all publish at once.
    if (payload && len == strlen(kAvailOnline) && memcmp(payload, kAvailOnline, len) == 0) {
      uint32_t delay = randomMs(_republishDelayMaxMs);
#ifdef HAS_LOG
      _log.info("Home Assistant is online, publishing discovery in %u ms", (unsigned)delay);
#endif
      _republishAt = nowMs() + delay;
      _republishPending = true;
    }
    return;
  }

  for (const CommandRoute& route : _commands) {
    if (route.topic == topic) {
      std::string p = payload ? std::string(reinterpret_cast<const char*>(payload), len) : std::string();
#ifdef HAS_LOG
      _log.debug("Command for %s: %s", route.object_id.c_str(), p.c_str());
#endif
      if (_onCommand) {
        _onCommand(route.object_id.c_str(), p.c_str());
      }
#ifdef HAS_LOG
      else {
        _log.warning("Command for %s received, but no handler is set", route.object_id.c_str());
      }
#endif
      return;
    }
  }
}

void HaDiscovery::registerCommandTopic(const std::string& topic, const char* object_id) {
  for (CommandRoute& route : _commands) {
    if (route.object_id == object_id) {
      if (route.topic == topic) {
        return;
      }
      route.topic = topic;
      if (canPublishNow()) {
        _transport.subscribe(topic.c_str(), 1);
      }
      return;
    }
  }
  _commands.push_back({topic, object_id});
  if (canPublishNow()) {
    _transport.subscribe(topic.c_str(), 1);
  }
}

void HaDiscovery::unregisterCommandTopics(const char* object_id) {
  for (auto it = _commands.begin(); it != _commands.end();) {
    if (it->object_id == object_id) {
      it = _commands.erase(it);
    } else {
      ++it;
    }
  }
}

void HaDiscovery::publishAvailabilityOnline(bool retained, uint8_t qos) {
  if (!hasDevice()) {
#ifdef HAS_LOG
    _log.warning("Skipping availability online, setDevice() has not been called");
#endif
    return;
  }
  std::string topic = buildDefaultAvailabilityTopic();
  _transport.publish(topic.c_str(), reinterpret_cast<const uint8_t*>(kAvailOnline), strlen(kAvailOnline), retained, qos);
}

void HaDiscovery::publishAvailabilityOffline(bool retained, uint8_t qos) {
  if (!hasDevice()) {
    return;
  }
  std::string topic = buildDefaultAvailabilityTopic();
#ifdef HAS_LOG
  _log.info("Publishing availability offline to %s", topic.c_str());
#endif
  _transport.publish(topic.c_str(), reinterpret_cast<const uint8_t*>(kAvailOffline), strlen(kAvailOffline), retained, qos);
}

bool HaDiscovery::publishSensorDiscovery(const HaSensorConfig& cfg, bool retained, uint8_t qos) {
  if (!hasDevice() || !cfg.common.object_id) {
    return false;
  }

  std::string json = buildSensorConfigJson(cfg);
  if (json.empty()) {
#ifdef HAS_LOG
    _log.error("Could not build discovery config for %s (out of memory?)", cfg.common.object_id);
#endif
    return false;
  }

  return storeAndPublishConfig("sensor", cfg.common.object_id, json.c_str(), retained, qos);
}

bool HaDiscovery::publishSwitchDiscovery(const HaSwitchConfig& cfg, bool retained, uint8_t qos) {
  if (!hasDevice() || !cfg.common.object_id) {
    return false;
  }

  std::string json = buildSwitchConfigJson(cfg);
  if (json.empty()) {
#ifdef HAS_LOG
    _log.error("Could not build discovery config for %s (out of memory?)", cfg.common.object_id);
#endif
    return false;
  }

  registerCommandTopic(cfg.command_topic_override
                         ? std::string(cfg.command_topic_override)
                         : buildDefaultCommandTopic(cfg.common.object_id),
                       cfg.common.object_id);
  return storeAndPublishConfig("switch", cfg.common.object_id, json.c_str(), retained, qos);
}

bool HaDiscovery::publishBinarySensorDiscovery(const HaBinarySensorConfig& cfg, bool retained, uint8_t qos) {
  if (!hasDevice() || !cfg.common.object_id) {
    return false;
  }

  std::string json = buildBinarySensorConfigJson(cfg);
  if (json.empty()) {
#ifdef HAS_LOG
    _log.error("Could not build discovery config for %s (out of memory?)", cfg.common.object_id);
#endif
    return false;
  }

  return storeAndPublishConfig("binary_sensor", cfg.common.object_id, json.c_str(), retained, qos);
}

bool HaDiscovery::publishButtonDiscovery(const HaButtonConfig& cfg, bool retained, uint8_t qos) {
  if (!hasDevice() || !cfg.common.object_id) {
    return false;
  }

  std::string json = buildButtonConfigJson(cfg);
  if (json.empty()) {
#ifdef HAS_LOG
    _log.error("Could not build discovery config for %s (out of memory?)", cfg.common.object_id);
#endif
    return false;
  }

  registerCommandTopic(cfg.command_topic_override
                         ? std::string(cfg.command_topic_override)
                         : buildDefaultCommandTopic(cfg.common.object_id),
                       cfg.common.object_id);
  return storeAndPublishConfig("button", cfg.common.object_id, json.c_str(), retained, qos);
}

bool HaDiscovery::removeEntity(const char* component, const char* object_id, uint8_t qos) {
  if (!hasDevice() || !component || !object_id) {
    return false;
  }

  std::string topic = buildConfigTopic(component, object_id);

  if (strcmp(component, "switch") == 0 || strcmp(component, "button") == 0) {
    unregisterCommandTopics(object_id);
  }

  for (auto it = _entities.begin(); it != _entities.end();) {
    if (it->component == component && it->object_id == object_id) {
      it = _entities.erase(it);
    } else {
      ++it;
    }
  }
  std::string stateTopic = buildDefaultStateTopic(object_id);
  for (auto it = _states.begin(); it != _states.end();) {
    if (it->topic == stateTopic) {
      it = _states.erase(it);
    } else {
      ++it;
    }
  }

  // Empty retained config payload removes entity in Home Assistant
  return _transport.publish(topic.c_str(), nullptr, 0, true, qos);
}

bool HaDiscovery::publishState(const char* object_id, const char* payload, bool retained, uint8_t qos) {
  if (!hasDevice() || !object_id || !payload) {
    return false;
  }

  std::string topic = buildDefaultStateTopic(object_id);

  // Remember the last state so it can be sent again after a reconnect or HA restart
  bool stored = false;
  for (StateEntry& st : _states) {
    if (st.topic == topic) {
      st.payload = payload;
      st.retained = retained;
      st.qos = qos;
      stored = true;
      break;
    }
  }
  if (!stored) {
    _states.push_back({topic, payload, retained, qos});
  }

  if (!canPublishNow()) {
#ifdef HAS_LOG
    _log.debug("Not connected yet, state for %s will be sent on connect", object_id);
#endif
    return true;
  }

#ifdef HAS_LOG
  _log.debug("Publishing state to %s: %s", topic.c_str(), payload);
#endif
  bool ok = _transport.publish(topic.c_str(),
                      reinterpret_cast<const uint8_t*>(payload),
                      strlen(payload),
                      retained,
                      qos);
#ifdef HAS_LOG
  if (!ok) {
    _log.error("Failed to publish state to %s", topic.c_str());
  }
#endif
  return ok;
}

bool HaDiscovery::publishStateSwitch(const char* object_id, bool on, bool retained, uint8_t qos) {
  return publishState(object_id, on ? kOn : kOff, retained, qos);
}

std::string HaDiscovery::buildConfigTopic(const char* component, const char* object_id) const {
  // homeassistant/<component>/<node_id>/<object_id>/config
  return _discoveryPrefix + "/" + component + "/" + _device.node_id + "/" + object_id + "/config";
}

std::string HaDiscovery::buildDefaultStateTopic(const char* object_id) const {
  // <base>/<node_id>/<object_id>/state
  return _baseTopicPrefix + "/" + _device.node_id + "/" + object_id + "/state";
}

std::string HaDiscovery::buildDefaultCommandTopic(const char* object_id) const {
  // <base>/<node_id>/<object_id>/set
  return _baseTopicPrefix + "/" + _device.node_id + "/" + object_id + "/set";
}

std::string HaDiscovery::buildBirthTopic() const {
  // Home Assistant publishes "online"/"offline" here
  return _discoveryPrefix + "/status";
}

bool HaDiscovery::storeAndPublishConfig(const char* component, const char* object_id,
                                        const char* json, bool retained, uint8_t qos) {
  std::string topic = buildConfigTopic(component, object_id);

  bool stored = false;
  for (EntityConfig& e : _entities) {
    if (e.component == component && e.object_id == object_id) {
      e.topic = topic;
      e.json = json;
      e.retained = retained;
      e.qos = qos;
      stored = true;
      break;
    }
  }
  if (!stored) {
    _entities.push_back({component, object_id, topic, json, retained, qos});
  }

  if (!canPublishNow()) {
#ifdef HAS_LOG
    _log.debug("Not connected yet, %s/%s will be published on connect", component, object_id);
#endif
    return true;
  }
  return publishConfigJson(topic.c_str(), json, retained, qos);
}

std::string HaDiscovery::buildDefaultAvailabilityTopic() const {
  // <base>/<node_id>/status
  return _baseTopicPrefix + "/" + _device.node_id + "/status";
}

void HaDiscovery::addDeviceObject(JsonDocument& doc) const {
  JsonObject dev = doc["dev"].to<JsonObject>();
  JsonArray ids = dev["ids"].to<JsonArray>();
  ids.add(_device.identifiers.empty() ? _device.node_id : _device.identifiers);
  if (!_device.name.empty()) {
    dev["name"] = _device.name;
  }
  if (!_device.manufacturer.empty()) {
    dev["mf"] = _device.manufacturer;
  }
  if (!_device.model.empty()) {
    dev["mdl"] = _device.model;
  }
  if (!_device.sw_version.empty()) {
    dev["sw"] = _device.sw_version;
  }
}

bool HaDiscovery::publishConfigJson(const char* topic, const char* json, bool retained, uint8_t qos) {
#ifdef HAS_LOG
  _log.debug("Publishing discovery config to %s", topic);
#endif
  bool ok = _transport.publish(topic,
                      reinterpret_cast<const uint8_t*>(json),
                      strlen(json),
                      retained,
                      qos);
#ifdef HAS_LOG
  if (!ok) {
    _log.error("Failed to publish discovery config to %s", topic);
  }
#endif
  return ok;
}

void HaDiscovery::addCommonFields(JsonDocument& doc, const char* component, const HaEntityCommon& common) const {
  if (common.use_device_name) {
    // null name: the entity is the device's main feature and takes the device name
    doc["name"] = nullptr;
  } else {
    doc["name"] = common.name ? common.name : common.object_id;
  }
  doc["uniq_id"] = _device.node_id + "_" + common.object_id;
  if (common.entity_id) {
    doc["def_ent_id"] = std::string(component) + "." + common.entity_id;
  }

  doc["avty_t"] = common.availability_topic_override
    ? std::string(common.availability_topic_override)
    : buildDefaultAvailabilityTopic();
  doc["pl_avail"] = kAvailOnline;
  doc["pl_not_avail"] = kAvailOffline;

  if (common.icon) {
    doc["ic"] = common.icon;
  }

  addDeviceObject(doc);

  JsonObject o = doc["o"].to<JsonObject>();
  o["name"] = _origin.name;
  if (!_origin.sw_version.empty()) {
    o["sw"] = _origin.sw_version;
  }
  if (!_origin.url.empty()) {
    o["url"] = _origin.url;
  }
}

std::string HaDiscovery::stateTopicFor(const HaEntityCommon& common) const {
  return common.state_topic_override
    ? std::string(common.state_topic_override)
    : buildDefaultStateTopic(common.object_id);
}

std::string HaDiscovery::serializeDoc(const JsonDocument& doc) {
  if (doc.overflowed()) {
    return std::string();
  }
  std::string out;
  out.resize(measureJson(doc));
  // std::string always has room for the terminator after size()
  serializeJson(doc, &out[0], out.size() + 1);
  return out;
}

std::string HaDiscovery::buildSensorConfigJson(const HaSensorConfig& cfg) const {
  JsonDocument doc;
  addCommonFields(doc, "sensor", cfg.common);
  doc["stat_t"] = stateTopicFor(cfg.common);
  if (cfg.unit_of_measurement) {
    doc["unit_of_meas"] = cfg.unit_of_measurement;
  }
  if (cfg.device_class) {
    doc["dev_cla"] = cfg.device_class;
  }
  if (cfg.state_class) {
    doc["stat_cla"] = cfg.state_class;
  }
  return serializeDoc(doc);
}

std::string HaDiscovery::buildSwitchConfigJson(const HaSwitchConfig& cfg) const {
  JsonDocument doc;
  addCommonFields(doc, "switch", cfg.common);
  doc["stat_t"] = stateTopicFor(cfg.common);
  doc["cmd_t"] = cfg.command_topic_override
    ? std::string(cfg.command_topic_override)
    : buildDefaultCommandTopic(cfg.common.object_id);
  doc["pl_on"] = cfg.payload_on ? cfg.payload_on : kOn;
  doc["pl_off"] = cfg.payload_off ? cfg.payload_off : kOff;
  return serializeDoc(doc);
}

std::string HaDiscovery::buildButtonConfigJson(const HaButtonConfig& cfg) const {
  JsonDocument doc;
  addCommonFields(doc, "button", cfg.common);
  doc["cmd_t"] = cfg.command_topic_override
    ? std::string(cfg.command_topic_override)
    : buildDefaultCommandTopic(cfg.common.object_id);
  doc["pl_prs"] = cfg.payload_press ? cfg.payload_press : kPress;
  return serializeDoc(doc);
}

std::string HaDiscovery::buildBinarySensorConfigJson(const HaBinarySensorConfig& cfg) const {
  JsonDocument doc;
  addCommonFields(doc, "binary_sensor", cfg.common);
  doc["stat_t"] = stateTopicFor(cfg.common);
  doc["pl_on"] = cfg.payload_on ? cfg.payload_on : kOn;
  doc["pl_off"] = cfg.payload_off ? cfg.payload_off : kOff;
  if (cfg.device_class) {
    doc["dev_cla"] = cfg.device_class;
  }
  return serializeDoc(doc);
}

bool HaDiscovery::pressButton(const char* object_id, const char* payload, bool retained, uint8_t qos) {
  if (!hasDevice() || !object_id) {
    return false;
  }

  std::string cmdTopic = buildDefaultCommandTopic(object_id);

  const char* p = payload ? payload : kPress;
  return _transport.publish(cmdTopic.c_str(),
                   reinterpret_cast<const uint8_t*>(p),
                   strlen(p),
                   retained,
                   qos);
}
