#include "HaDiscovery.h"
#include <stdio.h>
#include <stdlib.h>
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
static const char* kMigrate = "{\"migrate_discovery\":true}";
// Migrate messages and clears go out at QoS 0 (still retained). If one is lost, the
// old retained config is still on the broker and is handled again on the next
// connect. At QoS 1, AsyncMqttClient waits for each PUBACK before sending anything
// else, which held back a migration of 17 entities, and every command reply, for ~20 s.
static const uint8_t kMigrationQos = 0;

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
  _device.configuration_url = copy(dev.configuration_url);

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
  _transport.setWill(availabilityTopic().c_str(), kAvailOffline, true, 1);

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

void HaDiscovery::onMessage(HaMessageCallback cb) {
  _onMessage = cb;
}

void HaDiscovery::subscribe(const char* topic, uint8_t qos) {
  if (!topic || !*topic) {
    return;
  }
  for (Subscription& sub : _subscriptions) {
    if (sub.topic == topic) {
      sub.qos = qos;
      return;
    }
  }
  _subscriptions.push_back({topic, qos});
  if (_transport.connected() && !_connectPending) {
    _transport.subscribe(topic, qos);
  }
}

void HaDiscovery::setAvailabilityTopic(const char* topic) {
  _availabilityTopic = topic ? topic : "";
  if (hasDevice()) {
    _transport.setWill(availabilityTopic().c_str(), kAvailOffline, true, 1);
  }
  if (!_components.empty()) {
    _configDirty = true;
  }
}

void HaDiscovery::migrateEntityDiscovery(bool enable) {
  bool subscribeNow = enable && !_migrate && hasDevice() && _transport.connected() && !_connectPending;
  _migrate = enable;
  if (subscribeNow) {
    _transport.subscribe(buildMigrationFilter().c_str(), 1);
  }
}

void HaDiscovery::setRepublishDelay(uint32_t maxDelayMs) {
  _republishDelayMaxMs = maxDelayMs;
}

void HaDiscovery::setStateDelay(uint32_t delayMs) {
  _stateDelayMs = delayMs;
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
  processMigrationCleanups(now);

  if (_connectPending) {
    _connectPending = false;
    _republishPending = false;
    _statesPending = false;  // sent after the next device payload
    _publishRetry = false;  // a new connection, try right away
    // Subscriptions don't survive a clean-session reconnect
    subscribeAll();
    if (_migrate && hasDevice()) {
      // Wait for the retained single-component configs, so they're migrated before
      // Home Assistant sees the device payload with the same unique_ids
      startMigrationWait(now);
    } else {
      // A reconnect may follow a broker restart that lost retained messages,
      // and anything published while disconnected was only stored
      republishAll();
    }
  } else if (_migrationWait) {
    // Home Assistant must have every migrate message before the device payload
    if ((_migrationSynced || isDue(now, _migrationWaitUntil)) && !hasUnsentMigrations()) {
      _migrationWait = false;
      _republishPending = false;
      republishAll();
    }
  } else if (_configDirty) {
    if ((!_publishRetry || isDue(now, _publishRetryAt)) && !hasUnsentMigrations()) {
      republishAll();
    }
  } else if (_republishPending && isDue(now, _republishAt)) {
    _republishPending = false;
    republishAll();
  } else if (_statesPending && isDue(now, _statesAt)) {
    publishStoredStates();
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
  _migrationWait = false;
  _republishPending = false;
  // Discovery first, so Home Assistant has the entities before their state arrives.
  // If the transport doesn't take it (e.g. short of memory), hold the states and try again.
  if (!publishDevicePayload() || !publishAvailabilityOnline(true, 1)) {
    _configDirty = true;
    schedulePublishRetry(nowMs());
    return;
  }
  _publishRetry = false;
  _publishRetryDelayMs = 0;
  // Home Assistant subscribes to the state topics only once it has created the
  // entities, so states sent right away are lost on a new or migrated device
  if (_stateDelayMs > 0) {
    _statesPending = true;
    _statesAt = nowMs() + _stateDelayMs;
    return;
  }
  publishStoredStates();
}

void HaDiscovery::publishStoredStates() {
  _statesPending = false;
#ifdef HAS_LOG
  _log.debug("Publishing %u states", (unsigned)_states.size());
#endif
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
  // Only queue here; tick() handles it in loop().
  // Retained single-component configs arrive in one burst after subscribing, often more
  // than MAX_INCOMING, so they have their own limit. Dropping one would leave its entity
  // behind on the old topic.
  bool migration = isMigrationTopic(topic);
  _incomingLock.lock();
  size_t& count = migration ? _incomingMigrationCount : _incomingCount;
  if (count < (migration ? MAX_MIGRATION_INCOMING : MAX_INCOMING)) {
    count++;
    _incoming.push_back({topic, payload ? std::string(reinterpret_cast<const char*>(payload), len) : std::string()});
  }
#ifdef HAS_LOG
  else {
    _droppedIncoming++;
  }
#endif
  _incomingLock.unlock();
}

void HaDiscovery::processIncoming() {
  // Old configs that couldn't be read last time go first, to keep the order
  std::vector<IncomingMessage> messages;
  messages.swap(_deferredMigrations);
  _incomingLock.lock();
  if (messages.empty()) {
    messages.swap(_incoming);
  } else {
    messages.insert(messages.end(), _incoming.begin(), _incoming.end());
    _incoming.clear();
  }
  _incomingCount = 0;
  _incomingMigrationCount = 0;
#ifdef HAS_LOG
  uint32_t dropped = _droppedIncoming;
  _droppedIncoming = 0;
#endif
  _incomingLock.unlock();

#ifdef HAS_LOG
  if (dropped) {
    _log.warning("Dropped %u incoming messages, the queue was full", (unsigned)dropped);
  }
#endif

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

  if (_migrate && hasDevice() && msg.topic == buildMigrationSyncTopic()) {
    // Ignore one from an earlier connection
    if (_migrationWait && msg.payload == std::to_string(_migrationSyncToken)) {
      _migrationSynced = true;
    }
    return;
  }

  if (handleMigrationMessage(msg)) {
    return;
  }

  if (_onMessage) {
    _onMessage(msg.topic.c_str(), msg.payload.c_str());
  }

  for (const CommandRoute& route : _commands) {
    if (route.topic == msg.topic) {
#ifdef HAS_LOG
      _log.debug("Command for %s: %s", route.object_id.c_str(), msg.payload.c_str());
#endif
      if (!_onCommand) {
#ifdef HAS_LOG
        if (!_onMessage) {
          _log.warning("Command for %s received, but no handler is set", route.object_id.c_str());
        }
#endif
        return;
      }
      if (route.kind == CommandKind::Plain) {
        _onCommand(route.object_id.c_str(), msg.payload.c_str());
        return;
      }
      // A part of a default schema light: pass it on as a JSON schema command
      std::string json = lightCommandJson(route.kind, msg.payload);
      if (json.empty()) {
#ifdef HAS_LOG
        _log.warning("Ignoring light command for %s: '%s' isn't valid", route.object_id.c_str(), msg.payload.c_str());
#endif
        return;
      }
      _onCommand(route.object_id.c_str(), json.c_str());
      return;
    }
  }
}

std::string HaDiscovery::lightCommandJson(CommandKind kind, const std::string& payload) {
  auto clamp = [](long v, long lo, long hi) { return v < lo ? lo : (v > hi ? hi : v); };
  JsonDocument doc;
  const char* text = payload.c_str();
  char* end = nullptr;
  switch (kind) {
    case CommandKind::Plain:
      return payload;
    case CommandKind::LightState:
      doc["state"] = payload;
      break;
    case CommandKind::LightBrightness: {
      long v = strtol(text, &end, 10);
      if (end == text) {
        return std::string();
      }
      doc["brightness"] = clamp(v, 0, 65535);
      break;
    }
    case CommandKind::LightRgb: {
      int r, g, b;
      if (sscanf(text, "%d,%d,%d", &r, &g, &b) != 3) {
        return std::string();
      }
      JsonObject color = doc["color"].to<JsonObject>();
      color["r"] = clamp(r, 0, 255);
      color["g"] = clamp(g, 0, 255);
      color["b"] = clamp(b, 0, 255);
      break;
    }
    case CommandKind::LightColorTemp: {
      long v = strtol(text, &end, 10);
      if (end == text) {
        return std::string();
      }
      doc["color_temp"] = clamp(v, 0, 65535);
      break;
    }
  }
  return serializeDoc(doc);
}

// ---------------------------------------------------------------------------
// Migration from single-component discovery
// ---------------------------------------------------------------------------

bool HaDiscovery::isMigrationTopic(const char* topic) const {
  // Also called from the network task. _migrate, the prefix and node_id are set
  // before connecting, so they don't change under it.
  if (!_migrate || !hasDevice() || !topic) {
    return false;
  }
  // <discovery_prefix>/<component>/<node_id>/<object_id>/config
  size_t prefixLen = _discoveryPrefix.size();
  if (strncmp(topic, _discoveryPrefix.c_str(), prefixLen) != 0 || topic[prefixLen] != '/') {
    return false;
  }
  std::vector<std::string> parts;
  const char* p = topic + prefixLen + 1;
  while (true) {
    const char* slash = strchr(p, '/');
    parts.push_back(slash ? std::string(p, slash - p) : std::string(p));
    if (!slash) {
      break;
    }
    p = slash + 1;
  }
  return parts.size() == 4 && parts[0] != "device" && parts[1] == _device.node_id && parts[3] == "config";
}

void HaDiscovery::startMigrationWait(uint32_t now) {
  // There's no telling when the broker has sent all retained messages for a subscription.
  // But it handles a client's packets in order, so a message we publish after subscribing
  // comes back after them. subscribeAll() has just subscribed to the old topics.
  _migrationWait = true;
  _migrationSynced = false;
  _migrationSyncToken++;
  std::string topic = buildMigrationSyncTopic();
  std::string token = std::to_string(_migrationSyncToken);
  _transport.subscribe(topic.c_str(), 0);
  bool sent = _transport.publish(topic.c_str(), reinterpret_cast<const uint8_t*>(token.data()), token.size(), false, 0);
  // If it couldn't be sent, fall back to a short fixed wait
  _migrationWaitUntil = now + (sent ? MIGRATION_SYNC_TIMEOUT_MS : MIGRATION_WAIT_MS);
}

bool HaDiscovery::hasUniqueId(const std::string& uniqueId) const {
  JsonDocument filter;
  filter["uniq_id"] = true;
  for (const Component& c : _components) {
    JsonDocument doc;
    if (!deserializeJson(doc, c.json, DeserializationOption::Filter(filter)) && doc["uniq_id"] == uniqueId) {
      return true;
    }
  }
  return false;
}

bool HaDiscovery::handleMigrationMessage(const IncomingMessage& msg) {
  if (!isMigrationTopic(msg.topic.c_str())) {
    return false;
  }

  if (msg.payload.empty()) {
    return true;  // already cleared
  }

  bool isMigrateMessage = false;
  std::string uniqueId;
  {
    // Only keep the keys we need: an old config can be a few hundred bytes, and the
    // ESP8266 may be short of memory while it publishes the device payload
    JsonDocument filter;
    filter["uniq_id"] = true;
    filter["unique_id"] = true;
    filter["migrate_discovery"] = true;
    filter["migr_discvry"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, msg.payload, DeserializationOption::Filter(filter));
    if (err == DeserializationError::NoMemory) {
      if (msg.attempts + 1 < MIGRATION_PARSE_ATTEMPTS) {
        IncomingMessage retry = msg;
        retry.attempts++;
        _deferredMigrations.push_back(retry);
      }
#ifdef HAS_LOG
      else {
        _log.error("Couldn't read %s (out of memory), it's tried again on the next connect", msg.topic.c_str());
      }
#endif
      return true;
    }
    if (err) {
#ifdef HAS_LOG
      _log.warning("Not migrating %s: not valid JSON (%s)", msg.topic.c_str(), err.c_str());
#endif
      return true;
    }
    if (doc.is<JsonObject>()) {
      isMigrateMessage = doc["migrate_discovery"].is<bool>() || doc["migr_discvry"].is<bool>();
      if (doc["uniq_id"].is<const char*>()) {
        uniqueId = doc["uniq_id"].as<const char*>();
      } else if (doc["unique_id"].is<const char*>()) {
        uniqueId = doc["unique_id"].as<const char*>();
      }
    }
  }

  if (!isMigrateMessage) {
    // Only migrate entities the device payload takes over. Otherwise Home Assistant
    // would unload the entity and nothing would load it again, leaving it orphaned.
    // Left alone, the old entity keeps working next to the new one.
    if (uniqueId.empty() || !hasUniqueId(uniqueId)) {
#ifdef HAS_LOG
      _log.warning("Not migrating %s: no entity has its unique_id '%s'", msg.topic.c_str(), uniqueId.c_str());
#endif
      return true;
    }
#ifdef HAS_LOG
    _log.info("Migrating %s to device discovery", msg.topic.c_str());
#endif
  }

  for (const MigrationCleanup& c : _migrationCleanups) {
    if (c.topic == msg.topic) {
      return true;  // already handled, e.g. our own migrate message coming back
    }
  }

  uint32_t now = nowMs();
  if (isMigrateMessage) {
    // A migrate message from an earlier run that didn't get to clear it: just clear it
    _migrationCleanups.push_back({msg.topic, now + MIGRATION_CLEANUP_DELAY_MS, true});
    return true;
  }

  // Home Assistant unloads the entity but keeps its settings. The device payload,
  // published after this, loads it again from the component with the same unique_id.
  // Then the old topic is cleared; Home Assistant ignores it from now on.
  // If the transport doesn't take the migrate message, tick() tries again, and holds
  // the device payload until it's out.
  bool sent = publishRetained(msg.topic, kMigrate, kMigrationQos);
  if (sent) {
    _configDirty = true;
  }
  _migrationCleanups.push_back({msg.topic, now + (sent ? MIGRATION_CLEANUP_DELAY_MS : MIGRATION_RETRY_MS), sent});
  return true;
}

bool HaDiscovery::hasUnsentMigrations() const {
  if (!_deferredMigrations.empty()) {
    return true;
  }
  for (const MigrationCleanup& c : _migrationCleanups) {
    if (!c.sent) {
      return true;
    }
  }
  return false;
}

void HaDiscovery::processMigrationCleanups(uint32_t now) {
  if (_connectPending) {
    return;
  }
  // Clear old topics only once the device payload that took them over is out
  bool canClear = !_migrationWait && !_configDirty;
  for (auto it = _migrationCleanups.begin(); it != _migrationCleanups.end();) {
    if (!isDue(now, it->at) || (it->sent && !canClear)) {
      ++it;
      continue;
    }
    if (!it->sent) {
#ifdef HAS_LOG
      _log.info("Migrating %s to device discovery (retry)", it->topic.c_str());
#endif
      if (publishRetained(it->topic, kMigrate, kMigrationQos)) {
        it->sent = true;
        it->at = now + MIGRATION_CLEANUP_DELAY_MS;
        _configDirty = true;
      } else {
        it->at = now + MIGRATION_RETRY_MS;
      }
      ++it;
      continue;
    }
#ifdef HAS_LOG
    _log.info("Clearing migrated %s", it->topic.c_str());
#endif
    if (publishRetained(it->topic, std::string(), kMigrationQos)) {
      it = _migrationCleanups.erase(it);
    } else {
      it->at = now + MIGRATION_RETRY_MS;
      ++it;
    }
  }
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void HaDiscovery::subscribeAll() {
  _transport.subscribe(buildBirthTopic().c_str(), 1);
  if (_migrate && hasDevice()) {
    _transport.subscribe(buildMigrationFilter().c_str(), 1);
  }
  for (const CommandRoute& route : _commands) {
    _transport.subscribe(route.topic.c_str(), 1);
  }
  for (const Subscription& sub : _subscriptions) {
    _transport.subscribe(sub.topic.c_str(), sub.qos);
  }
}

void HaDiscovery::registerCommandTopic(const std::string& topic, const char* object_id, CommandKind kind) {
  // An entity can have several command topics (e.g. a cover's position); one route per topic
  for (CommandRoute& route : _commands) {
    if (route.topic == topic) {
      route.object_id = object_id;
      route.kind = kind;
      return;
    }
  }
  _commands.push_back({topic, object_id, kind});
  if (_transport.connected() && !_connectPending) {
    _transport.subscribe(topic.c_str(), 1);
  }
}

void HaDiscovery::setCommandTopics(const char* object_id, const std::vector<std::string>& topics,
                                   const std::vector<CommandKind>& kinds) {
  // Drop routes the entity no longer has, keep the rest (already subscribed), add new ones
  for (auto it = _commands.begin(); it != _commands.end();) {
    bool keep = it->object_id != object_id;
    for (const std::string& t : topics) {
      keep = keep || it->topic == t;
    }
    it = keep ? it + 1 : _commands.erase(it);
  }
  for (size_t i = 0; i < topics.size(); i++) {
    registerCommandTopic(topics[i], object_id, i < kinds.size() ? kinds[i] : CommandKind::Plain);
  }
}

std::string HaDiscovery::commandTopicFor(const char* override_topic, const char* object_id) const {
  return override_topic ? std::string(override_topic) : buildDefaultCommandTopic(object_id);
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

bool HaDiscovery::publishAvailabilityOnline(bool retained, uint8_t qos) {
  if (!hasDevice()) {
#ifdef HAS_LOG
    _log.warning("Skipping availability online, setDevice() has not been called");
#endif
    return false;
  }
  std::string topic = availabilityTopic();
  return _transport.publish(topic.c_str(), reinterpret_cast<const uint8_t*>(kAvailOnline), strlen(kAvailOnline), retained, qos);
}

void HaDiscovery::publishAvailabilityOffline(bool retained, uint8_t qos) {
  if (!hasDevice()) {
    return;
  }
  std::string topic = availabilityTopic();
#ifdef HAS_LOG
  _log.info("Publishing availability offline to %s", topic.c_str());
#endif
  _transport.publish(topic.c_str(), reinterpret_cast<const uint8_t*>(kAvailOffline), strlen(kAvailOffline), retained, qos);
}

bool HaDiscovery::canPublishStateNow() const {
  // While a connect or a device payload is pending, tick() sends the states right after it
  return _transport.connected() && !_connectPending && !_configDirty && !_migrationWait && !_statesPending;
}

bool HaDiscovery::publishState(const char* object_id, const char* payload, bool retained, uint8_t qos) {
  if (!hasDevice() || !object_id || !payload) {
    return false;
  }
  const Component* c = findComponent(object_id);
  if (c && !c->state_topics.empty() && !c->state_topics[0].empty()) {
    std::string topic = c->state_topics[0];
    return publishToTopic(topic, payload, retained, qos);
  }
  return publishToTopic(buildDefaultStateTopic(object_id), payload, retained, qos);
}

bool HaDiscovery::publishStateTopic(const char* topic, const char* payload, bool retained, uint8_t qos) {
  if (!topic || !*topic || !payload) {
    return false;
  }
  return publishToTopic(topic, payload, retained, qos);
}

const HaDiscovery::Component* HaDiscovery::findComponent(const char* object_id) const {
  for (const Component& c : _components) {
    if (c.object_id == object_id) {
      return &c;
    }
  }
  return nullptr;
}

bool HaDiscovery::publishToTopic(const std::string& topic, const char* payload, bool retained, uint8_t qos) {
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
    _log.debug("State for %s will be sent after the device payload", topic.c_str());
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
  return addComponent("sensor", cfg.common.object_id, std::move(json), stateTopicsFor(cfg.common));
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
  setCommandTopics(cfg.common.object_id, {commandTopicFor(cfg.command_topic_override, cfg.common.object_id)});
  return addComponent("switch", cfg.common.object_id, std::move(json), stateTopicsFor(cfg.common));
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
  return addComponent("binary_sensor", cfg.common.object_id, std::move(json), stateTopicsFor(cfg.common));
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
  setCommandTopics(cfg.common.object_id, {commandTopicFor(cfg.command_topic_override, cfg.common.object_id)});
  return addComponent("button", cfg.common.object_id, std::move(json));
}

bool HaDiscovery::addComponent(const char* component, const char* object_id, std::string json,
                               std::vector<std::string> state_topics) {
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
      c.state_topics = std::move(state_topics);
      _configDirty = true;
      return true;
    }
  }

  _components.push_back({component, object_id, std::move(json), std::move(state_topics)});
  _configDirty = true;
  return true;
}

bool HaDiscovery::removeEntity(const char* component, const char* object_id) {
  if (!component || !object_id) {
    return false;
  }

  bool found = false;
  std::vector<std::string> stateTopics;
  for (auto it = _components.begin(); it != _components.end(); ++it) {
    if (it->component == component && it->object_id == object_id) {
      stateTopics = std::move(it->state_topics);
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

  unregisterCommandTopics(object_id);

  // Drop its stored states (state, and position for a cover), also on override topics.
  // Override topics shared with other entities are kept.
  std::string prefix = _baseTopicPrefix + "/" + _device.node_id + "/" + object_id + "/";
  auto ownTopic = [&](const std::string& topic) {
    bool listed = false;
    for (const std::string& t : stateTopics) {
      listed = listed || (!t.empty() && t == topic);
    }
    if (!listed) {
      return false;
    }
    for (const Component& c : _components) {
      for (const std::string& t : c.state_topics) {
        if (t == topic) {
          return false;
        }
      }
    }
    return true;
  };
  for (auto it = _states.begin(); it != _states.end();) {
    if (it->topic.compare(0, prefix.size(), prefix) == 0 || ownTopic(it->topic)) {
      it = _states.erase(it);
    } else {
      ++it;
    }
  }
  return true;
}

bool HaDiscovery::removeDevice() {
  _components.clear();
  _removed.clear();
  _commands.clear();
  _states.clear();
  _migrationCleanups.clear();
  _deferredMigrations.clear();

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
  if (!publishRetained(buildDeviceTopic(), std::string(), 1)) {
    // tick() tries again
    _clearDeviceTopic = true;
    _configDirty = true;
    schedulePublishRetry(nowMs());
  }
  return true;
}

// ---------------------------------------------------------------------------
// Device payload
// ---------------------------------------------------------------------------

bool HaDiscovery::publishDevicePayload() {
  _configDirty = false;
  if (!hasDevice()) {
    return true;
  }

  std::string topic = buildDeviceTopic();

  if (!_removed.empty()) {
    // Removal stubs ({"p": ...}) remove components on current Home Assistant versions.
    // The payload without them below removes them on versions that treat the
    // payload as the full desired state.
    if (!publishRetained(topic, buildDevicePayload(true), 1)) {
      return false;  // keep the stubs for the retry
    }
    _removed.clear();
  }

  if (_components.empty()) {
    if (_clearDeviceTopic) {
      _clearDeviceTopic = false;
#ifdef HAS_LOG
      _log.info("No entities left, clearing %s", topic.c_str());
#endif
      if (!publishRetained(topic, std::string(), 1)) {
        _clearDeviceTopic = true;
        return false;
      }
    }
    // Nothing to announce yet. Don't clear the topic here, entities may be added later.
    return true;
  }

  std::string payload = buildDevicePayload(false);
  if (payload.empty()) {
#ifdef HAS_LOG
    _log.error("Could not build the device payload (out of memory?)");
#endif
    return false;
  }
#ifdef HAS_LOG
  _log.debug("Publishing device discovery to %s (%u bytes)", topic.c_str(), (unsigned)payload.size());
#endif
  return publishRetained(topic, payload, 1);
}

void HaDiscovery::schedulePublishRetry(uint32_t now) {
  uint32_t next = _publishRetryDelayMs == 0 ? PUBLISH_RETRY_MIN_MS : _publishRetryDelayMs * 2;
  _publishRetryDelayMs = next > PUBLISH_RETRY_MAX_MS ? PUBLISH_RETRY_MAX_MS : next;
  _publishRetry = true;
  _publishRetryAt = now + _publishRetryDelayMs;
#ifdef HAS_LOG
  _log.warning("Device payload or availability not published, trying again in %u ms", (unsigned)_publishRetryDelayMs);
#endif
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
  doc["avty_t"] = availabilityTopic();
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
// Topics
// ---------------------------------------------------------------------------

std::string HaDiscovery::buildDeviceTopic() const {
  // homeassistant/device/<node_id>/config
  return _discoveryPrefix + "/device/" + _device.node_id + "/config";
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

std::string HaDiscovery::availabilityTopic() const {
  return _availabilityTopic.empty() ? buildDefaultAvailabilityTopic() : _availabilityTopic;
}

std::string HaDiscovery::buildBirthTopic() const {
  // Home Assistant publishes "online"/"offline" here
  return _discoveryPrefix + "/status";
}

std::string HaDiscovery::buildMigrationFilter() const {
  // Single-component discovery topics of this node
  return _discoveryPrefix + "/+/" + _device.node_id + "/+/config";
}

std::string HaDiscovery::buildMigrationSyncTopic() const {
  return _baseTopicPrefix + "/" + _device.node_id + "/migration_sync";
}

std::string HaDiscovery::entityTopic(const char* override_topic, const char* object_id, const char* suffix) const {
  if (override_topic) {
    return override_topic;
  }
  return _baseTopicPrefix + "/" + _device.node_id + "/" + object_id + suffix;
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
  if (!_device.configuration_url.empty()) {
    dev["cu"] = _device.configuration_url;
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
  if (common.unique_id && *common.unique_id) {
    doc["uniq_id"] = common.unique_id;
  } else {
    doc["uniq_id"] = _device.node_id + "_" + common.object_id;
  }
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

  if (common.entity_category == HaEntityCategory::Diagnostic) {
    doc["ent_cat"] = "diagnostic";
  } else if (common.entity_category == HaEntityCategory::Config) {
    if (strcmp(component, "sensor") == 0 || strcmp(component, "binary_sensor") == 0) {
#ifdef HAS_LOG
      _log.warning("%s %s can't have entity category config, leaving it out", component, common.object_id);
#endif
    } else {
      doc["ent_cat"] = "config";
    }
  }
}

void HaDiscovery::addValueTemplate(JsonDocument& doc, const HaEntityCommon& common, const char* key) const {
  if (common.value_template && *common.value_template) {
    doc[key] = common.value_template;
  }
}

std::vector<std::string> HaDiscovery::stateTopicsFor(const HaEntityCommon& common) const {
  if (common.state_topic_override) {
    return {common.state_topic_override};
  }
  return {};
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
  addValueTemplate(doc, cfg.common);
  if (cfg.unit_of_measurement) {
    doc["unit_of_meas"] = cfg.unit_of_measurement;
  }
  if (cfg.device_class) {
    doc["dev_cla"] = cfg.device_class;
  }
  if (cfg.state_class) {
    doc["stat_cla"] = cfg.state_class;
  }
  if (cfg.expire_after) {
    doc["exp_aft"] = cfg.expire_after;
  }
  return serializeDoc(doc);
}

std::string HaDiscovery::buildSwitchConfigJson(const HaSwitchConfig& cfg) const {
  JsonDocument doc;
  addCommonFields(doc, "switch", cfg.common);
  doc["stat_t"] = stateTopicFor(cfg.common);
  addValueTemplate(doc, cfg.common);
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
  addValueTemplate(doc, cfg.common);
  doc["pl_on"] = cfg.payload_on ? cfg.payload_on : kOn;
  doc["pl_off"] = cfg.payload_off ? cfg.payload_off : kOff;
  if (cfg.device_class) {
    doc["dev_cla"] = cfg.device_class;
  }
  if (cfg.expire_after) {
    doc["exp_aft"] = cfg.expire_after;
  }
  return serializeDoc(doc);
}

// ---------------------------------------------------------------------------
// number, select, text, light, cover
// ---------------------------------------------------------------------------

bool HaDiscovery::addEntity(const char* component, const HaEntityCommon& common, std::string json,
                            std::vector<std::string> state_topics) {
  if (json.empty()) {
#ifdef HAS_LOG
    _log.error("Could not build discovery config for %s (out of memory?)", common.object_id);
#endif
    return false;
  }
  return addComponent(component, common.object_id, std::move(json), std::move(state_topics));
}

bool HaDiscovery::publishNumberDiscovery(const HaNumberConfig& cfg) {
  if (!hasDevice() || !cfg.common.object_id) {
    return false;
  }
  if (cfg.min > cfg.max || cfg.step <= 0) {
#ifdef HAS_LOG
    _log.error("number %s: min must be <= max and step > 0", cfg.common.object_id);
#endif
    return false;
  }
  setCommandTopics(cfg.common.object_id, {commandTopicFor(cfg.command_topic_override, cfg.common.object_id)});
  return addEntity("number", cfg.common, buildNumberConfigJson(cfg), stateTopicsFor(cfg.common));
}

bool HaDiscovery::publishSelectDiscovery(const HaSelectConfig& cfg) {
  if (!hasDevice() || !cfg.common.object_id) {
    return false;
  }
  if (cfg.options.empty()) {
#ifdef HAS_LOG
    _log.error("select %s needs at least one option", cfg.common.object_id);
#endif
    return false;
  }
  setCommandTopics(cfg.common.object_id, {commandTopicFor(cfg.command_topic_override, cfg.common.object_id)});
  return addEntity("select", cfg.common, buildSelectConfigJson(cfg), stateTopicsFor(cfg.common));
}

bool HaDiscovery::publishTextDiscovery(const HaTextConfig& cfg) {
  if (!hasDevice() || !cfg.common.object_id) {
    return false;
  }
  if (cfg.min_length > cfg.max_length || cfg.max_length > 255) {
#ifdef HAS_LOG
    _log.error("text %s: min_length must be <= max_length <= 255", cfg.common.object_id);
#endif
    return false;
  }
  setCommandTopics(cfg.common.object_id, {commandTopicFor(cfg.command_topic_override, cfg.common.object_id)});
  return addEntity("text", cfg.common, buildTextConfigJson(cfg), stateTopicsFor(cfg.common));
}

bool HaDiscovery::publishLightDiscovery(const HaLightConfig& cfg) {
  if (!hasDevice() || !cfg.common.object_id) {
    return false;
  }
  const char* id = cfg.common.object_id;
  if (cfg.schema == HaLightSchema::Json) {
#ifdef HAS_LOG
    if (cfg.common.value_template) {
      _log.warning("light %s: value_template is only used with HaLightSchema::Default", id);
    }
#endif
    setCommandTopics(id, {commandTopicFor(cfg.command_topic_override, id)});
    return addEntity("light", cfg.common, buildLightConfigJson(cfg), stateTopicsFor(cfg.common));
  }

  // Default schema: a topic pair per part. state_topics holds the state topics
  // in the order state, brightness, rgb, color temperature ("" if not used).
  std::vector<std::string> commands{commandTopicFor(cfg.command_topic_override, id)};
  std::vector<CommandKind> kinds{CommandKind::LightState};
  std::vector<std::string> states{stateTopicFor(cfg.common), "", "", ""};
  if (cfg.color_mode != HaLightColorMode::OnOff) {
    commands.push_back(entityTopic(cfg.brightness_command_topic_override, id, "/brightness/set"));
    kinds.push_back(CommandKind::LightBrightness);
    states[1] = entityTopic(cfg.brightness_state_topic_override, id, "/brightness");
  }
  if (cfg.color_mode == HaLightColorMode::Rgb) {
    commands.push_back(entityTopic(cfg.rgb_command_topic_override, id, "/rgb/set"));
    kinds.push_back(CommandKind::LightRgb);
    states[2] = entityTopic(cfg.rgb_state_topic_override, id, "/rgb");
  }
  if (cfg.color_mode == HaLightColorMode::ColorTemp) {
    commands.push_back(entityTopic(cfg.color_temp_command_topic_override, id, "/color_temp/set"));
    kinds.push_back(CommandKind::LightColorTemp);
    states[3] = entityTopic(cfg.color_temp_state_topic_override, id, "/color_temp");
  }
  setCommandTopics(id, commands, kinds);
  return addEntity("light", cfg.common, buildLightConfigJson(cfg), std::move(states));
}

bool HaDiscovery::publishCoverDiscovery(const HaCoverConfig& cfg) {
  if (!hasDevice() || !cfg.common.object_id) {
    return false;
  }
  std::vector<std::string> topics{commandTopicFor(cfg.command_topic_override, cfg.common.object_id)};
  if (cfg.has_position) {
    topics.push_back(_baseTopicPrefix + "/" + _device.node_id + "/" + cfg.common.object_id + "/set_position");
  }
  setCommandTopics(cfg.common.object_id, topics);
  return addEntity("cover", cfg.common, buildCoverConfigJson(cfg), stateTopicsFor(cfg.common));
}

std::string HaDiscovery::buildNumberConfigJson(const HaNumberConfig& cfg) const {
  JsonDocument doc;
  addCommonFields(doc, "number", cfg.common);
  doc["stat_t"] = stateTopicFor(cfg.common);
  addValueTemplate(doc, cfg.common);
  doc["cmd_t"] = commandTopicFor(cfg.command_topic_override, cfg.common.object_id);
  doc["min"] = cfg.min;
  doc["max"] = cfg.max;
  doc["step"] = cfg.step;
  if (cfg.mode == HaNumberMode::Box) {
    doc["mode"] = "box";
  } else if (cfg.mode == HaNumberMode::Slider) {
    doc["mode"] = "slider";
  }
  if (cfg.unit_of_measurement) {
    doc["unit_of_meas"] = cfg.unit_of_measurement;
  }
  if (cfg.device_class) {
    doc["dev_cla"] = cfg.device_class;
  }
  return serializeDoc(doc);
}

std::string HaDiscovery::buildSelectConfigJson(const HaSelectConfig& cfg) const {
  JsonDocument doc;
  addCommonFields(doc, "select", cfg.common);
  doc["stat_t"] = stateTopicFor(cfg.common);
  addValueTemplate(doc, cfg.common);
  doc["cmd_t"] = commandTopicFor(cfg.command_topic_override, cfg.common.object_id);
  JsonArray ops = doc["ops"].to<JsonArray>();
  for (const char* option : cfg.options) {
    if (option) {
      ops.add(option);
    }
  }
  return serializeDoc(doc);
}

std::string HaDiscovery::buildTextConfigJson(const HaTextConfig& cfg) const {
  JsonDocument doc;
  addCommonFields(doc, "text", cfg.common);
  doc["stat_t"] = stateTopicFor(cfg.common);
  addValueTemplate(doc, cfg.common);
  doc["cmd_t"] = commandTopicFor(cfg.command_topic_override, cfg.common.object_id);
  if (cfg.min_length != 0) {
    doc["min"] = cfg.min_length;
  }
  if (cfg.max_length != 255) {
    doc["max"] = cfg.max_length;
  }
  if (cfg.mode == HaTextMode::Password) {
    doc["mode"] = "password";
  }
  if (cfg.pattern) {
    doc["ptrn"] = cfg.pattern;
  }
  return serializeDoc(doc);
}

std::string HaDiscovery::buildLightConfigJson(const HaLightConfig& cfg) const {
  JsonDocument doc;
  addCommonFields(doc, "light", cfg.common);

  if (cfg.schema == HaLightSchema::Default) {
    // Home Assistant works out the color modes from the topics that are set
    const char* id = cfg.common.object_id;
    doc["stat_t"] = stateTopicFor(cfg.common);
    doc["cmd_t"] = commandTopicFor(cfg.command_topic_override, id);
    addValueTemplate(doc, cfg.common, "stat_val_tpl");
    if (cfg.color_mode != HaLightColorMode::OnOff) {
      doc["brightness_state_topic"] = entityTopic(cfg.brightness_state_topic_override, id, "/brightness");
      doc["brightness_command_topic"] = entityTopic(cfg.brightness_command_topic_override, id, "/brightness/set");
      if (cfg.brightness_scale != 255) {
        doc["brightness_scale"] = cfg.brightness_scale;
      }
    }
    if (cfg.color_mode == HaLightColorMode::Rgb) {
      doc["rgb_state_topic"] = entityTopic(cfg.rgb_state_topic_override, id, "/rgb");
      doc["rgb_command_topic"] = entityTopic(cfg.rgb_command_topic_override, id, "/rgb/set");
    }
    if (cfg.color_mode == HaLightColorMode::ColorTemp) {
      doc["color_temp_state_topic"] = entityTopic(cfg.color_temp_state_topic_override, id, "/color_temp");
      doc["color_temp_command_topic"] = entityTopic(cfg.color_temp_command_topic_override, id, "/color_temp/set");
      doc["color_temp_kelvin"] = true;
      doc["min_kelvin"] = cfg.min_kelvin;
      doc["max_kelvin"] = cfg.max_kelvin;
    }
    return serializeDoc(doc);
  }

  doc["schema"] = "json";
  doc["stat_t"] = stateTopicFor(cfg.common);
  doc["cmd_t"] = commandTopicFor(cfg.command_topic_override, cfg.common.object_id);
  JsonArray modes = doc["supported_color_modes"].to<JsonArray>();
  switch (cfg.color_mode) {
    case HaLightColorMode::OnOff:
      modes.add("onoff");
      break;
    case HaLightColorMode::Brightness:
      modes.add("brightness");
      break;
    case HaLightColorMode::ColorTemp:
      modes.add("color_temp");
      doc["color_temp_kelvin"] = true;
      doc["min_kelvin"] = cfg.min_kelvin;
      doc["max_kelvin"] = cfg.max_kelvin;
      break;
    case HaLightColorMode::Rgb:
      modes.add("rgb");
      // Without this, Home Assistant folds brightness into the RGB values and
      // never sends a separate "brightness"
      doc["brightness"] = true;
      break;
  }
  if (cfg.color_mode != HaLightColorMode::OnOff && cfg.brightness_scale != 255) {
    doc["brightness_scale"] = cfg.brightness_scale;
  }
  return serializeDoc(doc);
}

std::string HaDiscovery::buildCoverConfigJson(const HaCoverConfig& cfg) const {
  JsonDocument doc;
  addCommonFields(doc, "cover", cfg.common);
  doc["stat_t"] = stateTopicFor(cfg.common);
  addValueTemplate(doc, cfg.common);
  doc["cmd_t"] = commandTopicFor(cfg.command_topic_override, cfg.common.object_id);
  if (cfg.device_class) {
    doc["dev_cla"] = cfg.device_class;
  }
  if (cfg.payload_open) {
    doc["pl_open"] = cfg.payload_open;
  }
  if (cfg.payload_close) {
    doc["pl_cls"] = cfg.payload_close;
  }
  if (cfg.payload_stop) {
    if (*cfg.payload_stop) {
      doc["pl_stop"] = cfg.payload_stop;
    } else {
      doc["pl_stop"] = nullptr;  // the cover can't stop
    }
  }
  if (cfg.has_position) {
    std::string base = _baseTopicPrefix + "/" + _device.node_id + "/" + cfg.common.object_id;
    doc["pos_t"] = base + "/position";
    doc["set_pos_t"] = base + "/set_position";
  }
  return serializeDoc(doc);
}

// ---------------------------------------------------------------------------
// State helpers
// ---------------------------------------------------------------------------

bool HaDiscovery::publishStateNumber(const char* object_id, double value, uint8_t decimals, bool retained, uint8_t qos) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%.*f", decimals > 9 ? 9 : decimals, value);
  return publishState(object_id, buf, retained, qos);
}

bool HaDiscovery::publishStateLight(const char* object_id, const HaLightState& state, bool retained, uint8_t qos) {
  if (!hasDevice() || !object_id) {
    return false;
  }
  const Component* c = findComponent(object_id);
  if (c && c->component == "light" && c->state_topics.size() == 4) {
    // Default schema: each part on its own topic. Copy the topics, publishing may
    // run into code that changes _components.
    std::vector<std::string> topics = c->state_topics;
    bool ok = publishToTopic(topics[0], state.on ? kOn : kOff, retained, qos);
    char buf[16];
    if (state.brightness >= 0 && !topics[1].empty()) {
      snprintf(buf, sizeof(buf), "%d", state.brightness);
      ok = publishToTopic(topics[1], buf, retained, qos) && ok;
    }
    if (state.r >= 0 && !topics[2].empty()) {
      snprintf(buf, sizeof(buf), "%d,%d,%d", state.r, state.g, state.b);
      ok = publishToTopic(topics[2], buf, retained, qos) && ok;
    }
    if (state.color_temp_kelvin >= 0 && !topics[3].empty()) {
      snprintf(buf, sizeof(buf), "%d", state.color_temp_kelvin);
      ok = publishToTopic(topics[3], buf, retained, qos) && ok;
    }
    return ok;
  }

  JsonDocument doc;
  doc["state"] = state.on ? kOn : kOff;
  if (state.brightness >= 0) {
    doc["brightness"] = state.brightness;
  }
  if (state.r >= 0) {
    doc["color_mode"] = "rgb";
    JsonObject color = doc["color"].to<JsonObject>();
    color["r"] = state.r;
    color["g"] = state.g;
    color["b"] = state.b;
  } else if (state.color_temp_kelvin >= 0) {
    doc["color_mode"] = "color_temp";
    doc["color_temp"] = state.color_temp_kelvin;
  } else if (state.brightness >= 0) {
    doc["color_mode"] = "brightness";
  } else {
    doc["color_mode"] = "onoff";
  }
  std::string json = serializeDoc(doc);
  return publishState(object_id, json.c_str(), retained, qos);
}

bool HaDiscovery::publishStatePosition(const char* object_id, int position, bool retained, uint8_t qos) {
  if (!hasDevice() || !object_id) {
    return false;
  }
  if (position < 0) {
    position = 0;
  } else if (position > 100) {
    position = 100;
  }
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", position);
  return publishToTopic(_baseTopicPrefix + "/" + _device.node_id + "/" + object_id + "/position", buf, retained, qos);
}

bool HaDiscovery::parseLightCommand(const char* payload, HaLightCommand& out) {
  out = HaLightCommand();
  if (!payload) {
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, payload) || !doc.is<JsonObject>()) {
    return false;
  }
  if (doc["state"].is<const char*>()) {
    out.has_state = true;
    out.on = strcmp(doc["state"].as<const char*>(), kOn) == 0;
  }
  if (doc["brightness"].is<int>()) {
    out.has_brightness = true;
    out.brightness = doc["brightness"].as<uint16_t>();
  }
  if (doc["color_temp"].is<int>()) {
    out.has_color_temp = true;
    out.color_temp_kelvin = doc["color_temp"].as<uint16_t>();
  }
  if (doc["color"].is<JsonObject>()) {
    JsonObject color = doc["color"];
    if (color["r"].is<int>() && color["g"].is<int>() && color["b"].is<int>()) {
      out.has_color = true;
      out.r = color["r"].as<uint8_t>();
      out.g = color["g"].as<uint8_t>();
      out.b = color["b"].as<uint8_t>();
    }
  }
  if (doc["transition"].is<float>()) {
    out.has_transition = true;
    out.transition = doc["transition"].as<float>();
  }
  return true;
}
