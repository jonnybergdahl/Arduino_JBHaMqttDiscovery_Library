#include "HaDiscovery.h"
#include <stdio.h>
#include <string.h>
#include <ArduinoJson.h>

#if __has_include(<jblogger.h>)
#include <jblogger.h>
#define HAS_LOG
#endif

static const char* kAvailOnline = "online";
static const char* kAvailOffline = "offline";
static const char* kOn = "ON";
static const char* kOff = "OFF";
static const char* kPress = "PRESS";


HaDiscovery::HaDiscovery(MqttTransport& transport,
                         const char* discovery_prefix,
                         const char* base_topic_prefix)
  : t(transport),
    discoveryPrefix(discovery_prefix ? discovery_prefix : "homeassistant"),
    baseTopicPrefix(base_topic_prefix ? base_topic_prefix : "devices"),
    log("HaDiscovery") {
  t.setLogger(&log);
  t.setOnConnect(&HaDiscovery::onTransportConnectThunk, this);
}

void HaDiscovery::setLogLevel(LogLevel level) {
  log.setLogLevel(level);
}

void HaDiscovery::setDevice(const HaDeviceInfo& dev) {
  device = dev;
}

void HaDiscovery::tick() {
  t.tick();
}

void HaDiscovery::onTransportConnectThunk(void* ctx) {
  static_cast<HaDiscovery*>(ctx)->onTransportConnect();
}

void HaDiscovery::onTransportConnect() {
#ifdef HAS_LOG
  log.info("MQTT Transport connected");
#endif
  // Default behavior: publish availability online on connect.
  // Optional enhancement: re-publish all registered discovery configs.
  publishAvailabilityOnline(true, 1);
}

void HaDiscovery::publishAvailabilityOnline(bool retained, uint8_t qos) {
  char topic[TOPIC_BUF];
  buildDefaultAvailabilityTopic(topic, sizeof(topic));
  t.publish(topic, reinterpret_cast<const uint8_t*>(kAvailOnline), strlen(kAvailOnline), retained, qos);
}

void HaDiscovery::publishAvailabilityOffline(bool retained, uint8_t qos) {
  char topic[TOPIC_BUF];
  buildDefaultAvailabilityTopic(topic, sizeof(topic));
#ifdef HAS_LOG
  log.info("Publishing availability offline to %s", topic);
#endif
  t.publish(topic, reinterpret_cast<const uint8_t*>(kAvailOffline), strlen(kAvailOffline), retained, qos);
}

bool HaDiscovery::publishSensorDiscovery(const HaSensorConfig& cfg, bool retained, uint8_t qos) {
  if (!device.node_id || !cfg.common.object_id) return false;

  char topic[TOPIC_BUF];
  buildConfigTopic(topic, sizeof(topic), "sensor", cfg.common.object_id);

  char json[JSON_BUF];
  if (!buildSensorConfigJson(json, sizeof(json), cfg)) return false;

  return publishConfigJson(topic, json, retained, qos);
}

bool HaDiscovery::publishSwitchDiscovery(const HaSwitchConfig& cfg, bool retained, uint8_t qos) {
  if (!device.node_id || !cfg.common.object_id) return false;

  char topic[TOPIC_BUF];
  buildConfigTopic(topic, sizeof(topic), "switch", cfg.common.object_id);

  char json[JSON_BUF];
  if (!buildSwitchConfigJson(json, sizeof(json), cfg)) return false;

  return publishConfigJson(topic, json, retained, qos);
}

bool HaDiscovery::publishBinarySensorDiscovery(const HaBinarySensorConfig& cfg, bool retained, uint8_t qos) {
	if (!device.node_id || !cfg.common.object_id) return false;

	char topic[TOPIC_BUF];
	buildConfigTopic(topic, sizeof(topic), "binary_sensor", cfg.common.object_id);

	char json[JSON_BUF];
	if (!buildBinarySensorConfigJson(json, sizeof(json), cfg)) return false;

	return publishConfigJson(topic, json, retained, qos);
}

bool HaDiscovery::publishButtonDiscovery(const HaButtonConfig& cfg, bool retained, uint8_t qos) {
	if (!device.node_id || !cfg.common.object_id) return false;

	char topic[TOPIC_BUF];
	buildConfigTopic(topic, sizeof(topic), "button", cfg.common.object_id);

	char json[JSON_BUF];
	if (!buildButtonConfigJson(json, sizeof(json), cfg)) return false;

	return publishConfigJson(topic, json, retained, qos);
}

bool HaDiscovery::removeEntity(const char* component, const char* object_id, uint8_t qos) {
  if (!device.node_id || !component || !object_id) return false;

  char topic[TOPIC_BUF];
  buildConfigTopic(topic, sizeof(topic), component, object_id);

  // Empty retained config payload removes entity in Home Assistant
  return t.publish(topic, nullptr, 0, true, qos);
}

bool HaDiscovery::publishState(const char* object_id, const char* payload, bool retained, uint8_t qos) {
  if (!device.node_id || !object_id || !payload) return false;

  char topic[TOPIC_BUF];
  buildDefaultStateTopic(topic, sizeof(topic), object_id);

#ifdef HAS_LOG
  log.debug("Publishing state to %s: %s", topic, payload);
#endif
  bool ok = t.publish(topic,
                      reinterpret_cast<const uint8_t*>(payload),
                      strlen(payload),
                      retained,
                      qos);
#ifdef HAS_LOG
  if (!ok) log.error("Failed to publish state to %s", topic);
#endif
  return ok;
}

bool HaDiscovery::publishStateSwitch(const char* object_id, bool on, bool retained, uint8_t qos) {
  return publishState(object_id, on ? kOn : kOff, retained, qos);
}

void HaDiscovery::buildConfigTopic(char* out, size_t outLen, const char* component, const char* object_id) const {
  // homeassistant/<component>/<node_id>/<object_id>/config
  snprintf(out, outLen, "%s/%s/%s/%s/config",
           discoveryPrefix, component, device.node_id, object_id);
}

void HaDiscovery::buildDefaultStateTopic(char* out, size_t outLen, const char* object_id) const {
  // <base>/<node_id>/<object_id>/state
  snprintf(out, outLen, "%s/%s/%s/state", baseTopicPrefix, device.node_id, object_id);
}

void HaDiscovery::buildDefaultCommandTopic(char* out, size_t outLen, const char* object_id) const {
  // <base>/<node_id>/<object_id>/set
  snprintf(out, outLen, "%s/%s/%s/set", baseTopicPrefix, device.node_id, object_id);
}

void HaDiscovery::buildDefaultAvailabilityTopic(char* out, size_t outLen) const {
  // <base>/<node_id>/status
  snprintf(out, outLen, "%s/%s/status", baseTopicPrefix, device.node_id);
}

bool HaDiscovery::publishConfigJson(const char* topic, const char* json, bool retained, uint8_t qos) {
#ifdef HAS_LOG
  log.debug("Publishing discovery config to %s", topic);
#endif
  bool ok = t.publish(topic,
                      reinterpret_cast<const uint8_t*>(json),
                      strlen(json),
                      retained,
                      qos);
#ifdef HAS_LOG
  if (!ok) log.error("Failed to publish discovery config to %s", topic);
#endif
  return ok;
}

bool HaDiscovery::buildSensorConfigJson(char* out, size_t outLen, const HaSensorConfig& cfg) const {
  if (!out || outLen == 0) return false;

  // Topics
  char stateTopic[TOPIC_BUF];
  if (cfg.common.state_topic_override) {
    strncpy(stateTopic, cfg.common.state_topic_override, sizeof(stateTopic));
    stateTopic[sizeof(stateTopic) - 1] = '\0';
  } else {
    buildDefaultStateTopic(stateTopic, sizeof(stateTopic), cfg.common.object_id);
  }

  char availTopic[TOPIC_BUF];
  if (cfg.common.availability_topic_override) {
    strncpy(availTopic, cfg.common.availability_topic_override, sizeof(availTopic));
    availTopic[sizeof(availTopic) - 1] = '\0';
  } else {
    buildDefaultAvailabilityTopic(availTopic, sizeof(availTopic));
  }

  JsonDocument doc;
  doc["name"] = cfg.common.name ? cfg.common.name : cfg.common.object_id;
  char uniq[96];
  snprintf(uniq, sizeof(uniq), "%s_%s", device.node_id, cfg.common.object_id);
  doc["uniq_id"] = uniq;

  doc["stat_t"] = stateTopic;
  doc["avty_t"] = availTopic;
  doc["pl_avail"] = kAvailOnline;
  doc["pl_not_avail"] = kAvailOffline;

  if (cfg.common.icon) doc["icon"] = cfg.common.icon;
  if (cfg.unit_of_measurement) doc["unit_of_meas"] = cfg.unit_of_measurement;
  if (cfg.device_class) doc["dev_cla"] = cfg.device_class;
  if (cfg.state_class) doc["stat_cla"] = cfg.state_class;

  // Device object
  JsonObject dev = doc["dev"].to<JsonObject>();
  JsonArray ids = dev["ids"].to<JsonArray>();
  ids.add(device.identifiers ? device.identifiers : device.node_id);
  if (device.name) dev["name"] = device.name;
  if (device.manufacturer) dev["mf"] = device.manufacturer;
  if (device.model) dev["mdl"] = device.model;
  if (device.sw_version) dev["sw"] = device.sw_version;

  size_t n = serializeJson(doc, out, outLen);
  if (n == 0 || n >= outLen) return false;
  return true;
}

bool HaDiscovery::buildSwitchConfigJson(char* out, size_t outLen, const HaSwitchConfig& cfg) const {
  if (!out || outLen == 0) return false;

  // Topics
  char stateTopic[TOPIC_BUF];
  if (cfg.common.state_topic_override) {
    strncpy(stateTopic, cfg.common.state_topic_override, sizeof(stateTopic));
    stateTopic[sizeof(stateTopic) - 1] = '\0';
  } else {
    buildDefaultStateTopic(stateTopic, sizeof(stateTopic), cfg.common.object_id);
  }

  char cmdTopic[TOPIC_BUF];
  if (cfg.command_topic_override) {
    strncpy(cmdTopic, cfg.command_topic_override, sizeof(cmdTopic));
    cmdTopic[sizeof(cmdTopic) - 1] = '\0';
  } else {
    buildDefaultCommandTopic(cmdTopic, sizeof(cmdTopic), cfg.common.object_id);
  }

  char availTopic[TOPIC_BUF];
  if (cfg.common.availability_topic_override) {
    strncpy(availTopic, cfg.common.availability_topic_override, sizeof(availTopic));
    availTopic[sizeof(availTopic) - 1] = '\0';
  } else {
    buildDefaultAvailabilityTopic(availTopic, sizeof(availTopic));
  }

  const char* pOn = cfg.payload_on ? cfg.payload_on : kOn;
  const char* pOff = cfg.payload_off ? cfg.payload_off : kOff;

  JsonDocument doc;
  doc["name"] = cfg.common.name ? cfg.common.name : cfg.common.object_id;
  char uniq[96];
  snprintf(uniq, sizeof(uniq), "%s_%s", device.node_id, cfg.common.object_id);
  doc["uniq_id"] = uniq;
  doc["stat_t"] = stateTopic;
  doc["cmd_t"] = cmdTopic;
  doc["pl_on"] = pOn;
  doc["pl_off"] = pOff;
  doc["avty_t"] = availTopic;
  doc["pl_avail"] = kAvailOnline;
  doc["pl_not_avail"] = kAvailOffline;
  if (cfg.common.icon) doc["icon"] = cfg.common.icon;

  JsonObject dev = doc["dev"].to<JsonObject>();
  JsonArray ids = dev["ids"].to<JsonArray>();
  ids.add(device.identifiers ? device.identifiers : device.node_id);
  if (device.name) dev["name"] = device.name;
  if (device.manufacturer) dev["mf"] = device.manufacturer;
  if (device.model) dev["mdl"] = device.model;
  if (device.sw_version) dev["sw"] = device.sw_version;

  size_t n = serializeJson(doc, out, outLen);
  if (n == 0 || n >= outLen) return false;
  return true;
}

bool HaDiscovery::buildButtonConfigJson(char* out, size_t outLen, const HaButtonConfig& cfg) const {
  if (!out || outLen == 0) return false;

  // command topic
  char cmdTopic[TOPIC_BUF];
  if (cfg.command_topic_override) {
    strncpy(cmdTopic, cfg.command_topic_override, sizeof(cmdTopic));
    cmdTopic[sizeof(cmdTopic) - 1] = '\0';
  } else {
    buildDefaultCommandTopic(cmdTopic, sizeof(cmdTopic), cfg.common.object_id);
  }

  // availability topic
  char availTopic[TOPIC_BUF];
  if (cfg.common.availability_topic_override) {
    strncpy(availTopic, cfg.common.availability_topic_override, sizeof(availTopic));
    availTopic[sizeof(availTopic) - 1] = '\0';
  } else {
    buildDefaultAvailabilityTopic(availTopic, sizeof(availTopic));
  }

  const char* pPress = cfg.payload_press ? cfg.payload_press : kPress;

  JsonDocument doc;
  doc["name"] = cfg.common.name ? cfg.common.name : cfg.common.object_id;
  char uniq[96];
  snprintf(uniq, sizeof(uniq), "%s_%s", device.node_id, cfg.common.object_id);
  doc["uniq_id"] = uniq;
  doc["cmd_t"] = cmdTopic;
  doc["pl_prs"] = pPress;
  doc["avty_t"] = availTopic;
  doc["pl_avail"] = kAvailOnline;
  doc["pl_not_avail"] = kAvailOffline;
  if (cfg.common.icon) doc["icon"] = cfg.common.icon;

  JsonObject dev = doc["dev"].to<JsonObject>();
  JsonArray ids = dev["ids"].to<JsonArray>();
  ids.add(device.identifiers ? device.identifiers : device.node_id);
  if (device.name) dev["name"] = device.name;
  if (device.manufacturer) dev["mf"] = device.manufacturer;
  if (device.model) dev["mdl"] = device.model;
  if (device.sw_version) dev["sw"] = device.sw_version;

  size_t n = serializeJson(doc, out, outLen);
  if (n == 0 || n >= outLen) return false;
  return true;
}

// Added implementation using ArduinoJson
bool HaDiscovery::buildBinarySensorConfigJson(char* out, size_t outLen, const HaBinarySensorConfig& cfg) const {
  if (!out || outLen == 0) return false;

  // Topics
  char stateTopic[TOPIC_BUF];
  if (cfg.common.state_topic_override) {
    strncpy(stateTopic, cfg.common.state_topic_override, sizeof(stateTopic));
    stateTopic[sizeof(stateTopic) - 1] = '\0';
  } else {
    buildDefaultStateTopic(stateTopic, sizeof(stateTopic), cfg.common.object_id);
  }

  char availTopic[TOPIC_BUF];
  if (cfg.common.availability_topic_override) {
    strncpy(availTopic, cfg.common.availability_topic_override, sizeof(availTopic));
    availTopic[sizeof(availTopic) - 1] = '\0';
  } else {
    buildDefaultAvailabilityTopic(availTopic, sizeof(availTopic));
  }

  const char* pOn = cfg.payload_on ? cfg.payload_on : kOn;
  const char* pOff = cfg.payload_off ? cfg.payload_off : kOff;

  JsonDocument doc;
  doc["name"] = cfg.common.name ? cfg.common.name : cfg.common.object_id;
  char uniq[96];
  snprintf(uniq, sizeof(uniq), "%s_%s", device.node_id, cfg.common.object_id);
  doc["uniq_id"] = uniq;
  doc["stat_t"] = stateTopic;
  doc["avty_t"] = availTopic;
  doc["pl_avail"] = kAvailOnline;
  doc["pl_not_avail"] = kAvailOffline;
  doc["pl_on"] = pOn;
  doc["pl_off"] = pOff;
  if (cfg.common.icon) doc["icon"] = cfg.common.icon;
  if (cfg.device_class) doc["dev_cla"] = cfg.device_class;

  JsonObject dev = doc["dev"].to<JsonObject>();
  JsonArray ids = dev["ids"].to<JsonArray>();
  ids.add(device.identifiers ? device.identifiers : device.node_id);
  if (device.name) dev["name"] = device.name;
  if (device.manufacturer) dev["mf"] = device.manufacturer;
  if (device.model) dev["mdl"] = device.model;
  if (device.sw_version) dev["sw"] = device.sw_version;

  size_t n = serializeJson(doc, out, outLen);
  if (n == 0 || n >= outLen) return false;
  return true;
}

bool HaDiscovery::pressButton(const char* object_id, const char* payload, bool retained, uint8_t qos) {
	if (!device.node_id || !object_id) return false;

	char cmdTopic[TOPIC_BUF];
	buildDefaultCommandTopic(cmdTopic, sizeof(cmdTopic), object_id);

	const char* p = payload ? payload : kPress;
	return t.publish(cmdTopic,
					 reinterpret_cast<const uint8_t*>(p),
					 strlen(p),
					 retained,
					 qos);
}
