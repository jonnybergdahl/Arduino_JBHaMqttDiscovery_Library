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
static const char* kMigratePayload = "{\"migrate_discovery\":true}";

static bool isDue(uint32_t now, uint32_t at) {
  return static_cast<int32_t>(now - at) >= 0;
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

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

  if (!hasDevice()) {
    return;
  }

#ifdef HAS_LOG
  for (char c : _device.node_id) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) {
      _log.error("node_id '%s' may only contain letters, digits, '_' and '-'", _device.node_id.c_str());
      break;
    }
  }
#endif

  // The broker publishes this if the device drops off without disconnecting
  _transport.setWill(buildDefaultAvailabilityTopic().c_str(), kAvailOffline, true, 1);

  // The device block is part of the payload
  if (!_components.empty()) {
    _configDirty = true;
  }
}

void HaDiscovery::setOrigin(const char* name, const char* sw_version, const char* url) {
  _origin.name = name ? name : JBHAMQTTDISCOVERY_NAME;
  _origin.sw_version = sw_version ? sw_version : "";
  _origin.url = url ? url : "";
  if (!_components.empty()) {
    _configDirty = true;
  }
}

void HaDiscovery::onCommand(HaCommandCallback cb) {
  _onCommand = cb;
}

void HaDiscovery::setRepublishDelay(uint32_t maxDelayMs) {
  _republishDelayMaxMs = maxDelayMs;
}

void HaDiscovery::setMigrateLegacyDiscovery(bool enable, uint32_t clearDelayMs) {
  _migration = enable ? MigrationState::Pending : MigrationState::Off;
  _migrationClearDelayMs = clearDelayMs;
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void HaDiscovery::tick() {
  _transport.tick();
  processIncoming();

  if (!_transport.connected()) {
    return;
  }

  uint32_t now = nowMs();
  if (_connectPending) {
    _connectPending = false;
    _republishPending = false;
    // Subscriptions don't survive a clean-session reconnect
    subscribeAll();
    // A reconnect may follow a broker restart that lost retained messages,
    // and anything published while disconnected was only stored
    republishAll();
  } else if (_configDirty) {
    republishAll();
  } else if (_republishPending && isDue(now, _republishAt)) {
    _republishPending = false;
    republishAll();
  }

  if (_migration == MigrationState::WaitingToClear && isDue(now, _migrationClearAt)) {
    runMigrationClear();
  }
}

void HaDiscovery::republishAll() {
  if (!_transport.connected()) {
    return;
  }
#ifdef HAS_LOG
  _log.info("Publishing device discovery (%u entities) and %u states",
            (unsigned)_components.size(), (unsigned)_states.size());
#endif
  // Discovery first, so Home Assistant has the entities before their state arrives
  publishDevicePayload();
  publishAvailabilityOnline(true, 1);
  for (const StateEntry& st : _states) {
    _transport.publish(st.topic.c_str(),
                       reinterpret_cast<const uint8_t*>(st.payload.data()),
                       st.payload.size(),
                       st.retained,
                       st.qos);
  }
}

// ---------------------------------------------------------------------------
// Transport callbacks (may run in the network task)
// ---------------------------------------------------------------------------

void HaDiscovery::onTransportConnectThunk(void* ctx) {
  static_cast<HaDiscovery*>(ctx)->onTransportConnect();
}

void HaDiscovery::onTransportConnect() {
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
  // Only queue here; tick() handles it in loop()
  _incomingLock.lock();
  if (_incoming.size() < MAX_INCOMING) {
    _incoming.push_back({topic, payload ? std::string(reinterpret_cast<const char*>(payload), len) : std::string()});
  }
  _incomingLock.unlock();
}

void HaDiscovery::processIncoming() {
  std::vector<IncomingMessage> messages;
  _incomingLock.lock();
  messages.swap(_incoming);
  _incomingLock.unlock();

  for (const IncomingMessage& msg : messages) {
    handleMessage(msg);
  }
}

void HaDiscovery::handleMessage(const IncomingMessage& msg) {
  if (msg.topic == buildBirthTopic()) {
    // Home Assistant (re)started and has forgotten non-retained state.
    // Wait a random time so many devices don't all publish at once.
    if (msg.payload == kAvailOnline) {
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
    if (route.topic == msg.topic) {
#ifdef HAS_LOG
      _log.debug("Command for %s: %s", route.object_id.c_str(), msg.payload.c_str());
#endif
      if (_onCommand) {
        _onCommand(route.object_id.c_str(), msg.payload.c_str());
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

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void HaDiscovery::subscribeAll() {
  _transport.subscribe(buildBirthTopic().c_str(), 1);
  for (const CommandRoute& route : _commands) {
    _transport.subscribe(route.topic.c_str(), 1);
  }
}

void HaDiscovery::registerCommandTopic(const std::string& topic, const char* object_id) {
  bool subscribeNow = _transport.connected() && !_connectPending;
  for (CommandRoute& route : _commands) {
    if (route.object_id == object_id) {
      if (route.topic == topic) {
        return;
      }
      route.topic = topic;
      if (subscribeNow) {
        _transport.subscribe(topic.c_str(), 1);
      }
      return;
    }
  }
  _commands.push_back({topic, object_id});
  if (subscribeNow) {
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

// ---------------------------------------------------------------------------
// Availability and state
// ---------------------------------------------------------------------------

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

bool HaDiscovery::canPublishStateNow() const {
  // While a connect or a device payload is pending, tick() sends the states right after it
  return _transport.connected() && !_connectPending && !_configDirty;
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

  if (!canPublishStateNow()) {
#ifdef HAS_LOG
    _log.debug("State for %s will be sent after the device payload", object_id);
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

// ---------------------------------------------------------------------------
// Entities
// ---------------------------------------------------------------------------

bool HaDiscovery::publishSensorDiscovery(const HaSensorConfig& cfg) {
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
  return addComponent("sensor", cfg.common.object_id, std::move(json));
}

bool HaDiscovery::publishSwitchDiscovery(const HaSwitchConfig& cfg) {
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
  return addComponent("switch", cfg.common.object_id, std::move(json));
}

bool HaDiscovery::publishBinarySensorDiscovery(const HaBinarySensorConfig& cfg) {
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
  return addComponent("binary_sensor", cfg.common.object_id, std::move(json));
}

bool HaDiscovery::publishButtonDiscovery(const HaButtonConfig& cfg) {
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
  return addComponent("button", cfg.common.object_id, std::move(json));
}

bool HaDiscovery::addComponent(const char* component, const char* object_id, std::string json) {
  // Added again before the removal went out: no removal needed
  for (auto it = _removed.begin(); it != _removed.end();) {
    if (it->object_id == object_id) {
      it = _removed.erase(it);
    } else {
      ++it;
    }
  }
  _clearDeviceTopic = false;

  for (Component& c : _components) {
    if (c.object_id == object_id) {
      if (c.component == component && c.json == json) {
        return true;  // unchanged, nothing to publish
      }
#ifdef HAS_LOG
      if (c.component != component) {
        _log.warning("object_id %s changes from %s to %s; object_ids must be unique per device",
                     object_id, c.component.c_str(), component);
      }
#endif
      c.component = component;
      c.json = std::move(json);
      _configDirty = true;
      return true;
    }
  }

  _components.push_back({component, object_id, std::move(json)});
  _configDirty = true;
  return true;
}

bool HaDiscovery::removeEntity(const char* component, const char* object_id) {
  if (!component || !object_id) {
    return false;
  }

  bool found = false;
  for (auto it = _components.begin(); it != _components.end(); ++it) {
    if (it->component == component && it->object_id == object_id) {
      _components.erase(it);
      found = true;
      break;
    }
  }
  if (!found) {
    return false;
  }

  _removed.push_back({component, object_id, std::string()});
  if (_components.empty()) {
    _clearDeviceTopic = true;
  }
  _configDirty = true;

  if (strcmp(component, "switch") == 0 || strcmp(component, "button") == 0) {
    unregisterCommandTopics(object_id);
  }

  std::string stateTopic = buildDefaultStateTopic(object_id);
  for (auto it = _states.begin(); it != _states.end(); ++it) {
    if (it->topic == stateTopic) {
      _states.erase(it);
      break;
    }
  }
  return true;
}

bool HaDiscovery::removeDevice() {
  _components.clear();
  _removed.clear();
  _commands.clear();
  _states.clear();

  if (!hasDevice()) {
    return false;
  }
  if (!_transport.connected() || _connectPending) {
    // Clear the device topic once connected
    _clearDeviceTopic = true;
    _configDirty = true;
    return true;
  }
  _clearDeviceTopic = false;
  _configDirty = false;
#ifdef HAS_LOG
  _log.info("Removing device %s", _device.node_id.c_str());
#endif
  return publishRetained(buildDeviceTopic(), std::string(), 1);
}

// ---------------------------------------------------------------------------
// Device payload
// ---------------------------------------------------------------------------

void HaDiscovery::publishDevicePayload() {
  _configDirty = false;
  if (!hasDevice()) {
    return;
  }

  if (_migration == MigrationState::Pending && !_components.empty()) {
    runMigrationStart();
  }

  std::string topic = buildDeviceTopic();

  if (!_removed.empty()) {
    // Removal stubs ({"p": ...}) remove components on current Home Assistant versions.
    // The payload without them below removes them on versions that treat the
    // payload as the full desired state.
    publishRetained(topic, buildDevicePayload(true), 1);
    _removed.clear();
  }

  if (_components.empty()) {
    if (_clearDeviceTopic) {
      _clearDeviceTopic = false;
#ifdef HAS_LOG
      _log.info("No entities left, clearing %s", topic.c_str());
#endif
      publishRetained(topic, std::string(), 1);
    }
    // Nothing to announce yet. Don't clear the topic here, entities may be added later.
    return;
  }

  std::string payload = buildDevicePayload(false);
  if (payload.empty()) {
#ifdef HAS_LOG
    _log.error("Could not build the device payload (out of memory?)");
#endif
    return;
  }
#ifdef HAS_LOG
  _log.debug("Publishing device discovery to %s (%u bytes)", topic.c_str(), (unsigned)payload.size());
#endif
  publishRetained(topic, payload, 1);
}

std::string HaDiscovery::buildDevicePayload(bool withRemovalStubs) const {
  JsonDocument doc;
  addDeviceObject(doc);

  JsonObject o = doc["o"].to<JsonObject>();
  o["name"] = _origin.name;
  if (!_origin.sw_version.empty()) {
    o["sw"] = _origin.sw_version;
  }
  if (!_origin.url.empty()) {
    o["url"] = _origin.url;
  }

  // Shared by all components, unless a component sets its own
  doc["avty_t"] = buildDefaultAvailabilityTopic();
  doc["pl_avail"] = kAvailOnline;
  doc["pl_not_avail"] = kAvailOffline;

  JsonObject cmps = doc["cmps"].to<JsonObject>();
  for (const Component& c : _components) {
    cmps[c.object_id] = serialized(c.json.c_str(), c.json.size());
  }
  if (withRemovalStubs) {
    for (const Component& r : _removed) {
      cmps[r.object_id]["p"] = r.component;
    }
  }
  return serializeDoc(doc);
}

bool HaDiscovery::publishRetained(const std::string& topic, const std::string& payload, uint8_t qos) {
  bool ok = _transport.publish(topic.c_str(),
                               payload.empty() ? nullptr : reinterpret_cast<const uint8_t*>(payload.data()),
                               payload.size(),
                               true,
                               qos);
#ifdef HAS_LOG
  if (!ok) {
    _log.error("Failed to publish to %s", topic.c_str());
  }
#endif
  return ok;
}

// ---------------------------------------------------------------------------
// Migration from per-entity discovery topics
// ---------------------------------------------------------------------------

void HaDiscovery::runMigrationStart() {
#ifdef HAS_LOG
  _log.info("Migrating %u entities from per-entity discovery topics", (unsigned)_components.size());
#endif
  _legacyTopics.clear();
  for (const Component& c : _components) {
    std::string topic = buildLegacyConfigTopic(c.component, c.object_id);
    publishRetained(topic, kMigratePayload, 1);
    _legacyTopics.push_back(topic);
  }
  _migration = MigrationState::WaitingToClear;
  _migrationClearAt = nowMs() + _migrationClearDelayMs;
}

void HaDiscovery::runMigrationClear() {
#ifdef HAS_LOG
  _log.info("Clearing %u old per-entity discovery topics", (unsigned)_legacyTopics.size());
#endif
  for (const std::string& topic : _legacyTopics) {
    publishRetained(topic, std::string(), 1);
  }
  _legacyTopics.clear();
  _migration = MigrationState::Done;
}

// ---------------------------------------------------------------------------
// Topics
// ---------------------------------------------------------------------------

std::string HaDiscovery::buildDeviceTopic() const {
  // homeassistant/device/<node_id>/config
  return _discoveryPrefix + "/device/" + _device.node_id + "/config";
}

std::string HaDiscovery::buildLegacyConfigTopic(const std::string& component, const std::string& object_id) const {
  // homeassistant/<component>/<node_id>/<object_id>/config, used by older versions
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

std::string HaDiscovery::buildBirthTopic() const {
  // Home Assistant publishes "online"/"offline" here
  return _discoveryPrefix + "/status";
}

// ---------------------------------------------------------------------------
// Component configs
// ---------------------------------------------------------------------------

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

std::string HaDiscovery::sanitizeEntityId(const char* entity_id) const {
  std::string out(entity_id);
  bool changed = false;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
      changed = true;
    } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
      c = '_';
      changed = true;
    }
  }
#ifdef HAS_LOG
  if (changed) {
    _log.warning("entity_id '%s' changed to '%s'", entity_id, out.c_str());
  }
#else
  (void)changed;
#endif
  return out;
}

void HaDiscovery::addCommonFields(JsonDocument& doc, const char* component, const HaEntityCommon& common) const {
  doc["p"] = component;
  if (common.use_device_name) {
    // null name: the entity is the device's main feature and takes the device name
    doc["name"] = nullptr;
  } else {
    doc["name"] = common.name ? common.name : common.object_id;
  }
  doc["uniq_id"] = _device.node_id + "_" + common.object_id;
  if (common.entity_id && *common.entity_id) {
    doc["def_ent_id"] = std::string(component) + "." + sanitizeEntityId(common.entity_id);
  }

  // The default availability is shared at the root of the device payload
  if (common.availability_topic_override) {
    doc["avty_t"] = common.availability_topic_override;
    doc["pl_avail"] = kAvailOnline;
    doc["pl_not_avail"] = kAvailOffline;
  }

  if (common.icon) {
    doc["ic"] = common.icon;
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
