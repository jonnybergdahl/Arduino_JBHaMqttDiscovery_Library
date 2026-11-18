#include "HaDiscovery.h"
#include <stdio.h>
#include <string.h>
#include <ArduinoJson.h>

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

void HaDiscovery::onCommand(HaCommandCallback cb) {
  _onCommand = cb;
}

void HaDiscovery::tick() {
  _transport.tick();
}

void HaDiscovery::onTransportConnectThunk(void* ctx) {
  static_cast<HaDiscovery*>(ctx)->onTransportConnect();
}

void HaDiscovery::onTransportConnect() {
#ifdef HAS_LOG
  _log.info("MQTT Transport connected");
#endif
  // Default behavior: publish availability online on connect.
  // Optional enhancement: re-publish all registered discovery configs.
  publishAvailabilityOnline(true, 1);

  // Subscriptions don't survive a clean-session reconnect
  for (const CommandRoute& route : _commands) {
    _transport.subscribe(route.topic.c_str(), 1);
  }
}

void HaDiscovery::onTransportMessageThunk(void* ctx, const char* topic, const uint8_t* payload, size_t len) {
  static_cast<HaDiscovery*>(ctx)->onTransportMessage(topic, payload, len);
}

void HaDiscovery::onTransportMessage(const char* topic, const uint8_t* payload, size_t len) {
  if (!topic) {
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
      if (_transport.connected()) {
        _transport.subscribe(topic.c_str(), 1);
      }
      return;
    }
  }
  _commands.push_back({topic, object_id});
  if (_transport.connected()) {
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

  std::string topic = buildConfigTopic("sensor", cfg.common.object_id);

  char json[JSON_BUF];
  if (!buildSensorConfigJson(json, sizeof(json), cfg)) {
    return false;
  }

  return publishConfigJson(topic.c_str(), json, retained, qos);
}

bool HaDiscovery::publishSwitchDiscovery(const HaSwitchConfig& cfg, bool retained, uint8_t qos) {
  if (!hasDevice() || !cfg.common.object_id) {
    return false;
  }

  std::string topic = buildConfigTopic("switch", cfg.common.object_id);

  char json[JSON_BUF];
  if (!buildSwitchConfigJson(json, sizeof(json), cfg)) {
    return false;
  }

  registerCommandTopic(cfg.command_topic_override
                         ? std::string(cfg.command_topic_override)
                         : buildDefaultCommandTopic(cfg.common.object_id),
                       cfg.common.object_id);
  return publishConfigJson(topic.c_str(), json, retained, qos);
}

bool HaDiscovery::publishBinarySensorDiscovery(const HaBinarySensorConfig& cfg, bool retained, uint8_t qos) {
  if (!hasDevice() || !cfg.common.object_id) {
    return false;
  }

  std::string topic = buildConfigTopic("binary_sensor", cfg.common.object_id);

  char json[JSON_BUF];
  if (!buildBinarySensorConfigJson(json, sizeof(json), cfg)) {
    return false;
  }

  return publishConfigJson(topic.c_str(), json, retained, qos);
}

bool HaDiscovery::publishButtonDiscovery(const HaButtonConfig& cfg, bool retained, uint8_t qos) {
  if (!hasDevice() || !cfg.common.object_id) {
    return false;
  }

  std::string topic = buildConfigTopic("button", cfg.common.object_id);

  char json[JSON_BUF];
  if (!buildButtonConfigJson(json, sizeof(json), cfg)) {
    return false;
  }

  registerCommandTopic(cfg.command_topic_override
                         ? std::string(cfg.command_topic_override)
                         : buildDefaultCommandTopic(cfg.common.object_id),
                       cfg.common.object_id);
  return publishConfigJson(topic.c_str(), json, retained, qos);
}

bool HaDiscovery::removeEntity(const char* component, const char* object_id, uint8_t qos) {
  if (!hasDevice() || !component || !object_id) {
    return false;
  }

  std::string topic = buildConfigTopic(component, object_id);

  if (strcmp(component, "switch") == 0 || strcmp(component, "button") == 0) {
    unregisterCommandTopics(object_id);
  }

  // Empty retained config payload removes entity in Home Assistant
  return _transport.publish(topic.c_str(), nullptr, 0, true, qos);
}

bool HaDiscovery::publishState(const char* object_id, const char* payload, bool retained, uint8_t qos) {
  if (!hasDevice() || !object_id || !payload) {
    return false;
  }

  std::string topic = buildDefaultStateTopic(object_id);

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

bool HaDiscovery::buildSensorConfigJson(char* out, size_t outLen, const HaSensorConfig& cfg) const {
  if (!out || outLen == 0) {
    return false;
  }

  // Topics
  std::string stateTopic = cfg.common.state_topic_override
    ? cfg.common.state_topic_override
    : buildDefaultStateTopic(cfg.common.object_id);

  std::string availTopic = cfg.common.availability_topic_override
    ? cfg.common.availability_topic_override
    : buildDefaultAvailabilityTopic();

  JsonDocument doc;
  doc["name"] = cfg.common.name ? cfg.common.name : cfg.common.object_id;
  std::string uniq = _device.node_id + "_" + cfg.common.object_id;
  doc["uniq_id"] = uniq;

  doc["stat_t"] = stateTopic;
  doc["avty_t"] = availTopic;
  doc["pl_avail"] = kAvailOnline;
  doc["pl_not_avail"] = kAvailOffline;

  if (cfg.common.icon) {
    doc["icon"] = cfg.common.icon;
  }
  if (cfg.unit_of_measurement) {
    doc["unit_of_meas"] = cfg.unit_of_measurement;
  }
  if (cfg.device_class) {
    doc["dev_cla"] = cfg.device_class;
  }
  if (cfg.state_class) {
    doc["stat_cla"] = cfg.state_class;
  }

  addDeviceObject(doc);

  size_t n = serializeJson(doc, out, outLen);
  if (n == 0 || n >= outLen) {
    return false;
  }
  return true;
}

bool HaDiscovery::buildSwitchConfigJson(char* out, size_t outLen, const HaSwitchConfig& cfg) const {
  if (!out || outLen == 0) {
    return false;
  }

  // Topics
  std::string stateTopic = cfg.common.state_topic_override
    ? cfg.common.state_topic_override
    : buildDefaultStateTopic(cfg.common.object_id);

  std::string cmdTopic = cfg.command_topic_override
    ? cfg.command_topic_override
    : buildDefaultCommandTopic(cfg.common.object_id);

  std::string availTopic = cfg.common.availability_topic_override
    ? cfg.common.availability_topic_override
    : buildDefaultAvailabilityTopic();

  const char* pOn = cfg.payload_on ? cfg.payload_on : kOn;
  const char* pOff = cfg.payload_off ? cfg.payload_off : kOff;

  JsonDocument doc;
  doc["name"] = cfg.common.name ? cfg.common.name : cfg.common.object_id;
  std::string uniq = _device.node_id + "_" + cfg.common.object_id;
  doc["uniq_id"] = uniq;
  doc["stat_t"] = stateTopic;
  doc["cmd_t"] = cmdTopic;
  doc["pl_on"] = pOn;
  doc["pl_off"] = pOff;
  doc["avty_t"] = availTopic;
  doc["pl_avail"] = kAvailOnline;
  doc["pl_not_avail"] = kAvailOffline;
  if (cfg.common.icon) {
    doc["icon"] = cfg.common.icon;
  }

  addDeviceObject(doc);

  size_t n = serializeJson(doc, out, outLen);
  if (n == 0 || n >= outLen) {
    return false;
  }
  return true;
}

bool HaDiscovery::buildButtonConfigJson(char* out, size_t outLen, const HaButtonConfig& cfg) const {
  if (!out || outLen == 0) {
    return false;
  }

  // command topic
  std::string cmdTopic = cfg.command_topic_override
    ? cfg.command_topic_override
    : buildDefaultCommandTopic(cfg.common.object_id);

  // availability topic
  std::string availTopic = cfg.common.availability_topic_override
    ? cfg.common.availability_topic_override
    : buildDefaultAvailabilityTopic();

  const char* pPress = cfg.payload_press ? cfg.payload_press : kPress;

  JsonDocument doc;
  doc["name"] = cfg.common.name ? cfg.common.name : cfg.common.object_id;
  std::string uniq = _device.node_id + "_" + cfg.common.object_id;
  doc["uniq_id"] = uniq;
  doc["cmd_t"] = cmdTopic;
  doc["pl_prs"] = pPress;
  doc["avty_t"] = availTopic;
  doc["pl_avail"] = kAvailOnline;
  doc["pl_not_avail"] = kAvailOffline;
  if (cfg.common.icon) {
    doc["icon"] = cfg.common.icon;
  }

  addDeviceObject(doc);

  size_t n = serializeJson(doc, out, outLen);
  if (n == 0 || n >= outLen) {
    return false;
  }
  return true;
}

bool HaDiscovery::buildBinarySensorConfigJson(char* out, size_t outLen, const HaBinarySensorConfig& cfg) const {
  if (!out || outLen == 0) {
    return false;
  }

  // Topics
  std::string stateTopic = cfg.common.state_topic_override
    ? cfg.common.state_topic_override
    : buildDefaultStateTopic(cfg.common.object_id);

  std::string availTopic = cfg.common.availability_topic_override
    ? cfg.common.availability_topic_override
    : buildDefaultAvailabilityTopic();

  const char* pOn = cfg.payload_on ? cfg.payload_on : kOn;
  const char* pOff = cfg.payload_off ? cfg.payload_off : kOff;

  JsonDocument doc;
  doc["name"] = cfg.common.name ? cfg.common.name : cfg.common.object_id;
  std::string uniq = _device.node_id + "_" + cfg.common.object_id;
  doc["uniq_id"] = uniq;
  doc["stat_t"] = stateTopic;
  doc["avty_t"] = availTopic;
  doc["pl_avail"] = kAvailOnline;
  doc["pl_not_avail"] = kAvailOffline;
  doc["pl_on"] = pOn;
  doc["pl_off"] = pOff;
  if (cfg.common.icon) {
    doc["icon"] = cfg.common.icon;
  }
  if (cfg.device_class) {
    doc["dev_cla"] = cfg.device_class;
  }

  addDeviceObject(doc);

  size_t n = serializeJson(doc, out, outLen);
  if (n == 0 || n >= outLen) {
    return false;
  }
  return true;
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
