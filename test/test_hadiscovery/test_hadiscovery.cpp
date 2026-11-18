#include <unity.h>
#include <string>
#include <vector>
#include <cstring>
#include "HaDiscovery.h"
#include "transport/MqttTransport.h"
#include <ArduinoJson.h>

static const char* kDeviceTopic = "homeassistant/device/test_node/config";

// Mock MQTT Transport
class MockTransport : public MqttTransport {
public:
    struct Message {
        std::string topic;
        std::string payload;
        bool retained;
        uint8_t qos;
    };

    std::vector<Message> messages;
    std::vector<std::string> subscriptions;
    std::string willTopic;
    std::string willPayload;
    bool willRetained = false;
    bool isConnected = true;
    void (*onConnectCb)(void*) = nullptr;
    void* onConnectCtx = nullptr;
    MessageCallback onMessageCb = nullptr;
    void* onMessageCtx = nullptr;

    bool connect(const char* clientId = nullptr) override { return true; }

    bool subscribe(const char* topic, uint8_t qos = 0) override {
        subscriptions.push_back(topic);
        return true;
    }

    void setOnMessage(MessageCallback cb, void* ctx) override {
        onMessageCb = cb;
        onMessageCtx = ctx;
    }

    void setWill(const char* topic, const char* payload, bool retained = true, uint8_t qos = 1) override {
        willTopic = topic;
        willPayload = payload;
        willRetained = retained;
    }

    // Simulate a message arriving from the broker
    void deliver(const char* topic, const char* payload) {
        if (onMessageCb) {
            onMessageCb(onMessageCtx, topic, (const uint8_t*)payload, strlen(payload));
        }
    }

    // Simulate the connection coming up
    void fireConnect() {
        if (onConnectCb) {
            onConnectCb(onConnectCtx);
        }
    }

    bool connected() const override { return isConnected; }

    bool publish(const char* topic, const uint8_t* payload, size_t len, bool retained, uint8_t qos) override {
        Message msg;
        msg.topic = topic;
        if (payload && len > 0) {
            msg.payload = std::string((const char*)payload, len);
        }
        msg.retained = retained;
        msg.qos = qos;
        messages.push_back(msg);
        return true;
    }

    void setOnConnect(void (*cb)(void*), void* ctx) override {
        onConnectCb = cb;
        onConnectCtx = ctx;
    }

    void setServer(const char* host, uint16_t port, const char* user = nullptr, const char* pass = nullptr) override {}
    void setServer(const std::string& host, uint16_t port, const std::string& user = "", const std::string& pass = "") override {}

    void clear() {
        messages.clear();
        subscriptions.clear();
        willTopic.clear();
        willPayload.clear();
        isConnected = true;
    }

    // Messages published to a topic, in order
    std::vector<Message> on(const std::string& topic) const {
        std::vector<Message> out;
        for (const Message& m : messages) {
            if (m.topic == topic) {
                out.push_back(m);
            }
        }
        return out;
    }

    int indexOf(const std::string& topic) const {
        for (size_t i = 0; i < messages.size(); i++) {
            if (messages[i].topic == topic) {
                return (int)i;
            }
        }
        return -1;
    }
};

MockTransport transport;
HaDiscovery* discovery;
std::vector<std::pair<std::string, std::string>> commands;

void setUp(void) {
    transport.clear();
    commands.clear();
    discovery = new HaDiscovery(transport, "homeassistant", "devices");
    discovery->setLogLevel(LOG_LEVEL_NONE);

    HaDeviceInfo dev;
    dev.node_id = "test_node";
    dev.name = "Test Device";
    dev.identifiers = "test_mac";
    dev.manufacturer = "Manufacturer";
    dev.model = "Model X";
    dev.sw_version = "1.0.0";
    discovery->setDevice(dev);
}

void tearDown(void) {
    delete discovery;
}

// Parse the last device payload
static JsonDocument lastDevicePayload() {
    std::vector<MockTransport::Message> msgs = transport.on(kDeviceTopic);
    TEST_ASSERT_FALSE_MESSAGE(msgs.empty(), "No device payload published");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, msgs.back().payload);
    TEST_ASSERT_FALSE_MESSAGE(err, "Device payload is not valid JSON");
    return doc;
}

static void recordCommand(const char* object_id, const char* payload) {
    commands.push_back({object_id, payload});
}

static void addSensor(const char* id = "temp") {
    HaSensorConfig cfg;
    cfg.common.object_id = id;
    discovery->publishSensorDiscovery(cfg);
}

// ---------------------------------------------------------------------------
// Device payload
// ---------------------------------------------------------------------------

void test_sensor_component(void) {
    HaSensorConfig cfg;
    cfg.common.object_id = "temp";
    cfg.common.name = "Temperature";
    cfg.unit_of_measurement = "°C";
    cfg.device_class = "temperature";
    cfg.state_class = "measurement";
    TEST_ASSERT_TRUE(discovery->publishSensorDiscovery(cfg));
    TEST_ASSERT_EQUAL(0, transport.messages.size());  // sent from tick()

    discovery->tick();
    std::vector<MockTransport::Message> msgs = transport.on(kDeviceTopic);
    TEST_ASSERT_EQUAL(1, msgs.size());
    TEST_ASSERT_TRUE(msgs[0].retained);

    JsonDocument doc = lastDevicePayload();
    TEST_ASSERT_EQUAL_STRING("test_mac", doc["dev"]["ids"][0]);
    TEST_ASSERT_EQUAL_STRING("Test Device", doc["dev"]["name"]);
    TEST_ASSERT_EQUAL_STRING("Manufacturer", doc["dev"]["mf"]);
    TEST_ASSERT_EQUAL_STRING("Model X", doc["dev"]["mdl"]);
    TEST_ASSERT_EQUAL_STRING("1.0.0", doc["dev"]["sw"]);
    TEST_ASSERT_EQUAL_STRING("devices/test_node/status", doc["avty_t"]);
    TEST_ASSERT_EQUAL_STRING("online", doc["pl_avail"]);
    TEST_ASSERT_EQUAL_STRING("offline", doc["pl_not_avail"]);

    JsonObject c = doc["cmps"]["temp"];
    TEST_ASSERT_EQUAL_STRING("sensor", c["p"]);
    TEST_ASSERT_EQUAL_STRING("Temperature", c["name"]);
    TEST_ASSERT_EQUAL_STRING("test_node_temp", c["uniq_id"]);
    TEST_ASSERT_EQUAL_STRING("devices/test_node/temp/state", c["stat_t"]);
    TEST_ASSERT_EQUAL_STRING("°C", c["unit_of_meas"]);
    TEST_ASSERT_EQUAL_STRING("temperature", c["dev_cla"]);
    TEST_ASSERT_EQUAL_STRING("measurement", c["stat_cla"]);
    // dev and o are only allowed at the root
    TEST_ASSERT_FALSE(c["dev"].is<JsonObject>());
    TEST_ASSERT_FALSE(c["o"].is<JsonObject>());
    TEST_ASSERT_FALSE(c["avty_t"].is<const char*>());
}

void test_switch_component(void) {
    HaSwitchConfig cfg;
    cfg.common.object_id = "relay";
    discovery->publishSwitchDiscovery(cfg);
    discovery->tick();

    JsonDocument doc = lastDevicePayload();

    JsonObject c = doc["cmps"]["relay"];
    TEST_ASSERT_EQUAL_STRING("switch", c["p"]);
    TEST_ASSERT_EQUAL_STRING("relay", c["name"]);  // object_id when no name is set
    TEST_ASSERT_EQUAL_STRING("devices/test_node/relay/state", c["stat_t"]);
    TEST_ASSERT_EQUAL_STRING("devices/test_node/relay/set", c["cmd_t"]);
    TEST_ASSERT_EQUAL_STRING("ON", c["pl_on"]);
    TEST_ASSERT_EQUAL_STRING("OFF", c["pl_off"]);
}

void test_binary_sensor_component(void) {
    HaBinarySensorConfig cfg;
    cfg.common.object_id = "motion";
    cfg.device_class = "motion";
    cfg.payload_on = "1";
    discovery->publishBinarySensorDiscovery(cfg);
    discovery->tick();

    JsonDocument doc = lastDevicePayload();

    JsonObject c = doc["cmps"]["motion"];
    TEST_ASSERT_EQUAL_STRING("binary_sensor", c["p"]);
    TEST_ASSERT_EQUAL_STRING("motion", c["dev_cla"]);
    TEST_ASSERT_EQUAL_STRING("1", c["pl_on"]);
    TEST_ASSERT_EQUAL_STRING("OFF", c["pl_off"]);
}

void test_button_component(void) {
    HaButtonConfig cfg;
    cfg.common.object_id = "restart";
    discovery->publishButtonDiscovery(cfg);
    discovery->tick();

    JsonDocument doc = lastDevicePayload();

    JsonObject c = doc["cmps"]["restart"];
    TEST_ASSERT_EQUAL_STRING("button", c["p"]);
    TEST_ASSERT_EQUAL_STRING("devices/test_node/restart/set", c["cmd_t"]);
    TEST_ASSERT_EQUAL_STRING("PRESS", c["pl_prs"]);
    TEST_ASSERT_FALSE(c["stat_t"].is<const char*>());
}

void test_entities_share_one_payload(void) {
    addSensor("temp");
    addSensor("humidity");
    HaSwitchConfig sw;
    sw.common.object_id = "relay";
    discovery->publishSwitchDiscovery(sw);
    discovery->tick();

    TEST_ASSERT_EQUAL(1, transport.on(kDeviceTopic).size());
    JsonDocument doc = lastDevicePayload();
    JsonObject cmps = doc["cmps"];
    TEST_ASSERT_EQUAL(3, cmps.size());
    TEST_ASSERT_EQUAL_STRING("sensor", cmps["humidity"]["p"]);

    // Nothing changed: nothing more is published
    transport.clear();
    addSensor("temp");
    discovery->tick();
    TEST_ASSERT_EQUAL(0, transport.messages.size());
}

void test_changed_entity_republishes(void) {
    addSensor("temp");
    discovery->tick();
    transport.clear();

    HaSensorConfig cfg;
    cfg.common.object_id = "temp";
    cfg.unit_of_measurement = "K";
    discovery->publishSensorDiscovery(cfg);
    discovery->tick();
    TEST_ASSERT_EQUAL_STRING("K", lastDevicePayload()["cmps"]["temp"]["unit_of_meas"]);
}

void test_no_payload_without_entities(void) {
    discovery->tick();
    transport.fireConnect();
    discovery->tick();
    TEST_ASSERT_EQUAL(0, transport.on(kDeviceTopic).size());
}

void test_origin_defaults_to_library(void) {
    addSensor();
    discovery->tick();
    JsonDocument doc = lastDevicePayload();
    TEST_ASSERT_EQUAL_STRING(JBHAMQTTDISCOVERY_NAME, doc["o"]["name"]);
    TEST_ASSERT_EQUAL_STRING(JBHAMQTTDISCOVERY_VERSION, doc["o"]["sw"]);
    TEST_ASSERT_EQUAL_STRING(JBHAMQTTDISCOVERY_URL, doc["o"]["url"]);
}

void test_set_origin(void) {
    discovery->setOrigin("MyFirmware", "2.3.4");
    addSensor();
    discovery->tick();
    JsonDocument doc = lastDevicePayload();
    TEST_ASSERT_EQUAL_STRING("MyFirmware", doc["o"]["name"]);
    TEST_ASSERT_EQUAL_STRING("2.3.4", doc["o"]["sw"]);
    TEST_ASSERT_FALSE(doc["o"]["url"].is<const char*>());
}

void test_default_entity_id(void) {
    HaBinarySensorConfig cfg;
    cfg.common.object_id = "motion";
    cfg.common.entity_id = "hallway_motion";
    discovery->publishBinarySensorDiscovery(cfg);

    HaSensorConfig messy;
    messy.common.object_id = "temp";
    messy.common.entity_id = "Office Temp-1";
    discovery->publishSensorDiscovery(messy);

    addSensor("plain");
    discovery->tick();

    JsonDocument doc = lastDevicePayload();

    JsonObject cmps = doc["cmps"];
    TEST_ASSERT_EQUAL_STRING("binary_sensor.hallway_motion", cmps["motion"]["def_ent_id"]);
    TEST_ASSERT_EQUAL_STRING("sensor.office_temp_1", cmps["temp"]["def_ent_id"]);
    TEST_ASSERT_FALSE(cmps["plain"]["def_ent_id"].is<const char*>());
}

void test_use_device_name_sends_null_name(void) {
    HaSwitchConfig cfg;
    cfg.common.object_id = "relay";
    cfg.common.name = "ignored";
    cfg.common.use_device_name = true;
    discovery->publishSwitchDiscovery(cfg);
    discovery->tick();
    // The key must be present with a JSON null value, not left out
    TEST_ASSERT_NOT_EQUAL(std::string::npos, transport.on(kDeviceTopic).back().payload.find("\"name\":null"));
}

void test_icon_uses_abbreviation(void) {
    HaButtonConfig cfg;
    cfg.common.object_id = "restart";
    cfg.common.icon = "mdi:restart";
    discovery->publishButtonDiscovery(cfg);
    discovery->tick();
    JsonDocument doc = lastDevicePayload();
    JsonObject c = doc["cmps"]["restart"];
    TEST_ASSERT_EQUAL_STRING("mdi:restart", c["ic"]);
    TEST_ASSERT_FALSE(c["icon"].is<const char*>());
}

void test_availability_override_in_component(void) {
    HaSensorConfig cfg;
    cfg.common.object_id = "temp";
    cfg.common.availability_topic_override = "custom/avail";
    discovery->publishSensorDiscovery(cfg);
    discovery->tick();
    JsonDocument doc = lastDevicePayload();
    TEST_ASSERT_EQUAL_STRING("devices/test_node/status", doc["avty_t"]);
    TEST_ASSERT_EQUAL_STRING("custom/avail", doc["cmps"]["temp"]["avty_t"]);
}

void test_device_strings_are_copied(void) {
    char nodeId[16] = "temp_node";
    char mac[16] = "temp_mac";
    HaDeviceInfo dev;
    dev.node_id = nodeId;
    dev.identifiers = mac;
    discovery->setDevice(dev);

    // Overwrite the caller's buffers, like a local String going out of scope
    strcpy(nodeId, "XXXXXXXXX");
    strcpy(mac, "XXXXXXXX");

    addSensor();
    discovery->tick();
    std::vector<MockTransport::Message> msgs = transport.on("homeassistant/device/temp_node/config");
    TEST_ASSERT_EQUAL(1, msgs.size());
    JsonDocument doc;
    deserializeJson(doc, msgs[0].payload);
    TEST_ASSERT_EQUAL_STRING("temp_node_temp", doc["cmps"]["temp"]["uniq_id"]);
    TEST_ASSERT_EQUAL_STRING("temp_mac", doc["dev"]["ids"][0]);
    TEST_ASSERT_TRUE(doc["dev"]["name"].isNull());
}

void test_no_device_publishes_nothing(void) {
    HaDiscovery fresh(transport);
    fresh.setLogLevel(LOG_LEVEL_NONE);
    fresh.publishAvailabilityOnline();
    fresh.publishAvailabilityOffline();
    TEST_ASSERT_FALSE(fresh.publishState("temp", "1"));
    HaSensorConfig cfg;
    cfg.common.object_id = "temp";
    TEST_ASSERT_FALSE(fresh.publishSensorDiscovery(cfg));
    fresh.tick();
    TEST_ASSERT_EQUAL(0, transport.messages.size());
}

// ---------------------------------------------------------------------------
// State and availability
// ---------------------------------------------------------------------------

void test_publish_state(void) {
    discovery->publishState("temp", "23.5");
    TEST_ASSERT_EQUAL(1, transport.messages.size());
    TEST_ASSERT_EQUAL_STRING("devices/test_node/temp/state", transport.messages[0].topic.c_str());
    TEST_ASSERT_EQUAL_STRING("23.5", transport.messages[0].payload.c_str());
}

void test_publish_state_switch(void) {
    discovery->publishStateSwitch("relay", true);
    TEST_ASSERT_EQUAL(1, transport.messages.size());
    TEST_ASSERT_EQUAL_STRING("ON", transport.messages[0].payload.c_str());

    transport.clear();
    discovery->publishStateSwitch("relay", false);
    TEST_ASSERT_EQUAL_STRING("OFF", transport.messages[0].payload.c_str());
}

void test_state_waits_for_device_payload(void) {
    addSensor("temp");
    discovery->publishState("temp", "20.0");
    TEST_ASSERT_EQUAL(0, transport.messages.size());

    discovery->tick();
    int config = transport.indexOf(kDeviceTopic);
    int state = transport.indexOf("devices/test_node/temp/state");
    TEST_ASSERT_TRUE(config >= 0);
    TEST_ASSERT_TRUE(state > config);
}

void test_availability(void) {
    discovery->publishAvailabilityOnline();
    TEST_ASSERT_EQUAL(1, transport.messages.size());
    TEST_ASSERT_EQUAL_STRING("devices/test_node/status", transport.messages[0].topic.c_str());
    TEST_ASSERT_EQUAL_STRING("online", transport.messages[0].payload.c_str());

    transport.clear();
    discovery->publishAvailabilityOffline();
    TEST_ASSERT_EQUAL_STRING("offline", transport.messages[0].payload.c_str());
}

void test_will_set_by_set_device(void) {
    TEST_ASSERT_EQUAL_STRING("devices/test_node/status", transport.willTopic.c_str());
    TEST_ASSERT_EQUAL_STRING("offline", transport.willPayload.c_str());
    TEST_ASSERT_TRUE(transport.willRetained);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void test_switch_command_routed_in_tick(void) {
    discovery->onCommand(recordCommand);
    HaSwitchConfig cfg;
    cfg.common.object_id = "relay";
    discovery->publishSwitchDiscovery(cfg);

    TEST_ASSERT_EQUAL(1, transport.subscriptions.size());
    TEST_ASSERT_EQUAL_STRING("devices/test_node/relay/set", transport.subscriptions[0].c_str());

    transport.deliver("devices/test_node/relay/set", "ON");
    TEST_ASSERT_EQUAL(0, commands.size());  // queued, not handled in the network callback

    discovery->tick();
    TEST_ASSERT_EQUAL(1, commands.size());
    TEST_ASSERT_EQUAL_STRING("relay", commands[0].first.c_str());
    TEST_ASSERT_EQUAL_STRING("ON", commands[0].second.c_str());
}

void test_button_command_override_topic(void) {
    discovery->onCommand(recordCommand);
    HaButtonConfig cfg;
    cfg.common.object_id = "restart";
    cfg.command_topic_override = "custom/restart";
    discovery->publishButtonDiscovery(cfg);

    transport.deliver("devices/test_node/restart/set", "PRESS");
    discovery->tick();
    TEST_ASSERT_EQUAL(0, commands.size());

    transport.deliver("custom/restart", "PRESS");
    discovery->tick();
    TEST_ASSERT_EQUAL(1, commands.size());
    TEST_ASSERT_EQUAL_STRING("restart", commands[0].first.c_str());
}

void test_unknown_topic_ignored(void) {
    discovery->onCommand(recordCommand);
    transport.deliver("devices/test_node/other/set", "ON");
    discovery->tick();
    TEST_ASSERT_EQUAL(0, commands.size());
}

void test_incoming_queue_is_bounded(void) {
    discovery->onCommand(recordCommand);
    HaSwitchConfig cfg;
    cfg.common.object_id = "relay";
    discovery->publishSwitchDiscovery(cfg);
    for (int i = 0; i < 40; i++) {
        transport.deliver("devices/test_node/relay/set", "ON");
    }
    discovery->tick();
    TEST_ASSERT_EQUAL(16, commands.size());
}

void test_subscribe_waits_for_connect(void) {
    transport.isConnected = false;
    HaSwitchConfig cfg;
    cfg.common.object_id = "relay";
    discovery->publishSwitchDiscovery(cfg);
    TEST_ASSERT_EQUAL(0, transport.subscriptions.size());

    transport.isConnected = true;
    transport.fireConnect();
    discovery->tick();
    TEST_ASSERT_EQUAL(2, transport.subscriptions.size());
    TEST_ASSERT_EQUAL_STRING("homeassistant/status", transport.subscriptions[0].c_str());
    TEST_ASSERT_EQUAL_STRING("devices/test_node/relay/set", transport.subscriptions[1].c_str());

    // And again after a reconnect, without duplicating the route
    transport.subscriptions.clear();
    discovery->publishSwitchDiscovery(cfg);
    transport.fireConnect();
    discovery->tick();
    TEST_ASSERT_EQUAL(2, transport.subscriptions.size());
}

void test_removed_switch_stops_commands(void) {
    discovery->onCommand(recordCommand);
    HaSwitchConfig cfg;
    cfg.common.object_id = "relay";
    discovery->publishSwitchDiscovery(cfg);
    discovery->removeEntity("switch", "relay");

    transport.deliver("devices/test_node/relay/set", "ON");
    discovery->tick();
    TEST_ASSERT_EQUAL(0, commands.size());
}

// ---------------------------------------------------------------------------
// Connect, reconnect and Home Assistant restarts
// ---------------------------------------------------------------------------

static void addSensorAndState() {
    addSensor("temp");
    discovery->publishState("temp", "20.0");
}

// Device payload, then online, then the state
static void assertDeviceOnlineState(const char* state) {
    int config = transport.indexOf(kDeviceTopic);
    int online = transport.indexOf("devices/test_node/status");
    int st = transport.indexOf("devices/test_node/temp/state");
    TEST_ASSERT_TRUE_MESSAGE(config >= 0, "no device payload");
    TEST_ASSERT_TRUE_MESSAGE(online > config, "online not after device payload");
    TEST_ASSERT_TRUE_MESSAGE(st > online, "state not after online");
    TEST_ASSERT_EQUAL_STRING("online", transport.messages[online].payload.c_str());
    TEST_ASSERT_EQUAL_STRING(state, transport.messages[st].payload.c_str());
}

void test_publish_while_disconnected_is_deferred(void) {
    transport.isConnected = false;
    HaSensorConfig cfg;
    cfg.common.object_id = "temp";
    TEST_ASSERT_TRUE(discovery->publishSensorDiscovery(cfg));
    TEST_ASSERT_TRUE(discovery->publishState("temp", "20.0"));
    discovery->tick();
    TEST_ASSERT_EQUAL(0, transport.messages.size());

    transport.isConnected = true;
    transport.fireConnect();
    discovery->tick();
    assertDeviceOnlineState("20.0");
}

void test_reconnect_republishes_last_state(void) {
    addSensorAndState();
    discovery->tick();
    discovery->publishState("temp", "21.5");
    transport.clear();

    transport.fireConnect();
    discovery->tick();
    TEST_ASSERT_EQUAL(1, transport.on(kDeviceTopic).size());
    assertDeviceOnlineState("21.5");
}

void test_publish_during_pending_connect_sent_once(void) {
    transport.fireConnect();          // connect reported, tick() not run yet
    addSensorAndState();              // like setup() right after connect()
    TEST_ASSERT_EQUAL(0, transport.messages.size());

    discovery->tick();
    TEST_ASSERT_EQUAL(1, transport.on(kDeviceTopic).size());
    assertDeviceOnlineState("20.0");

    // Publishing now goes straight out
    size_t before = transport.messages.size();
    discovery->publishState("temp", "22.0");
    TEST_ASSERT_EQUAL(before + 1, transport.messages.size());
}

void test_birth_message_republishes_on_tick(void) {
    discovery->setRepublishDelay(0);
    addSensorAndState();
    discovery->tick();
    transport.clear();

    transport.deliver("homeassistant/status", "online");
    TEST_ASSERT_EQUAL(0, transport.messages.size());

    discovery->tick();
    assertDeviceOnlineState("20.0");

    // Only once
    transport.clear();
    discovery->tick();
    TEST_ASSERT_EQUAL(0, transport.messages.size());
}

void test_birth_message_waits_for_delay(void) {
    discovery->setRepublishDelay(60000);
    addSensorAndState();
    discovery->tick();
    transport.clear();

    // rand() could pick 0, so try a few birth messages until one is delayed
    for (int i = 0; i < 5; i++) {
        transport.deliver("homeassistant/status", "online");
        discovery->tick();
        if (transport.messages.empty()) {
            return;
        }
        transport.clear();
    }
    TEST_FAIL_MESSAGE("Republish was never delayed");
}

void test_birth_offline_ignored(void) {
    discovery->setRepublishDelay(0);
    addSensorAndState();
    discovery->tick();
    transport.clear();

    transport.deliver("homeassistant/status", "offline");
    discovery->tick();
    TEST_ASSERT_EQUAL(0, transport.messages.size());
}

// ---------------------------------------------------------------------------
// Removal and migration
// ---------------------------------------------------------------------------

void test_remove_entity_sends_stub_then_omits(void) {
    addSensor("temp");
    addSensor("humidity");
    discovery->publishState("temp", "20.0");
    discovery->tick();
    transport.clear();

    TEST_ASSERT_TRUE(discovery->removeEntity("sensor", "temp"));
    TEST_ASSERT_FALSE(discovery->removeEntity("sensor", "temp"));
    TEST_ASSERT_FALSE(discovery->removeEntity("switch", "humidity"));
    discovery->tick();

    std::vector<MockTransport::Message> msgs = transport.on(kDeviceTopic);
    TEST_ASSERT_EQUAL(2, msgs.size());

    JsonDocument stub;
    deserializeJson(stub, msgs[0].payload);
    TEST_ASSERT_EQUAL_STRING("sensor", stub["cmps"]["temp"]["p"]);
    TEST_ASSERT_EQUAL(1, stub["cmps"]["temp"].as<JsonObject>().size());
    TEST_ASSERT_EQUAL_STRING("test_node_humidity", stub["cmps"]["humidity"]["uniq_id"]);

    JsonDocument after;
    deserializeJson(after, msgs[1].payload);
    TEST_ASSERT_FALSE(after["cmps"]["temp"].is<JsonObject>());
    TEST_ASSERT_TRUE(after["cmps"]["humidity"].is<JsonObject>());

    // The removed entity's state isn't sent again
    TEST_ASSERT_EQUAL(-1, transport.indexOf("devices/test_node/temp/state"));
}

void test_remove_last_entity_clears_device_topic(void) {
    addSensor("temp");
    discovery->tick();
    transport.clear();

    discovery->removeEntity("sensor", "temp");
    discovery->tick();
    std::vector<MockTransport::Message> msgs = transport.on(kDeviceTopic);
    TEST_ASSERT_EQUAL(2, msgs.size());
    TEST_ASSERT_NOT_EQUAL(std::string::npos, msgs[0].payload.find("\"p\":\"sensor\""));
    TEST_ASSERT_TRUE(msgs[1].payload.empty());
    TEST_ASSERT_TRUE(msgs[1].retained);
}

void test_readd_before_tick_cancels_removal(void) {
    addSensor("temp");
    discovery->tick();
    transport.clear();

    discovery->removeEntity("sensor", "temp");
    addSensor("temp");
    discovery->tick();
    std::vector<MockTransport::Message> msgs = transport.on(kDeviceTopic);
    TEST_ASSERT_EQUAL(1, msgs.size());
    TEST_ASSERT_TRUE(lastDevicePayload()["cmps"]["temp"]["uniq_id"].is<const char*>());
}

void test_remove_device(void) {
    addSensorAndState();
    discovery->tick();
    transport.clear();

    TEST_ASSERT_TRUE(discovery->removeDevice());
    std::vector<MockTransport::Message> msgs = transport.on(kDeviceTopic);
    TEST_ASSERT_EQUAL(1, msgs.size());
    TEST_ASSERT_TRUE(msgs[0].payload.empty());
    TEST_ASSERT_TRUE(msgs[0].retained);

    // Nothing comes back on reconnect
    transport.clear();
    transport.fireConnect();
    discovery->tick();
    TEST_ASSERT_EQUAL(0, transport.on(kDeviceTopic).size());
    TEST_ASSERT_EQUAL(-1, transport.indexOf("devices/test_node/temp/state"));
}

void test_remove_device_while_disconnected(void) {
    addSensor("temp");
    transport.isConnected = false;
    TEST_ASSERT_TRUE(discovery->removeDevice());
    TEST_ASSERT_EQUAL(0, transport.messages.size());

    transport.isConnected = true;
    transport.fireConnect();
    discovery->tick();
    std::vector<MockTransport::Message> msgs = transport.on(kDeviceTopic);
    TEST_ASSERT_EQUAL(1, msgs.size());
    TEST_ASSERT_TRUE(msgs[0].payload.empty());
}

void test_migration_from_legacy_topics(void) {
    discovery->setMigrateLegacyDiscovery(true, 0);
    addSensor("temp");
    HaSwitchConfig sw;
    sw.common.object_id = "relay";
    discovery->publishSwitchDiscovery(sw);
    discovery->tick();

    const char* legacySensor = "homeassistant/sensor/test_node/temp/config";
    const char* legacySwitch = "homeassistant/switch/test_node/relay/config";

    // migrate_discovery first, then the device payload
    std::vector<MockTransport::Message> sensorMsgs = transport.on(legacySensor);
    TEST_ASSERT_TRUE(sensorMsgs.size() >= 1);
    JsonDocument migrate;
    deserializeJson(migrate, sensorMsgs[0].payload);
    TEST_ASSERT_TRUE(migrate["migrate_discovery"].as<bool>());
    TEST_ASSERT_TRUE(sensorMsgs[0].retained);
    TEST_ASSERT_TRUE(transport.indexOf(legacySensor) < transport.indexOf(kDeviceTopic));
    TEST_ASSERT_TRUE(transport.indexOf(legacySwitch) < transport.indexOf(kDeviceTopic));

    // With a 0 ms delay the old topics are cleared in the same tick
    sensorMsgs = transport.on(legacySensor);
    TEST_ASSERT_EQUAL(2, sensorMsgs.size());
    TEST_ASSERT_TRUE(sensorMsgs[1].payload.empty());
    TEST_ASSERT_TRUE(sensorMsgs[1].retained);
    TEST_ASSERT_EQUAL(2, transport.on(legacySwitch).size());

    // Only once
    transport.clear();
    transport.fireConnect();
    discovery->tick();
    TEST_ASSERT_EQUAL(0, transport.on(legacySensor).size());
}

void test_migration_waits_before_clearing(void) {
    discovery->setMigrateLegacyDiscovery(true, 60000);
    addSensor("temp");
    discovery->tick();
    std::vector<MockTransport::Message> msgs = transport.on("homeassistant/sensor/test_node/temp/config");
    TEST_ASSERT_EQUAL(1, msgs.size());
    TEST_ASSERT_FALSE(msgs[0].payload.empty());
}

// ---------------------------------------------------------------------------
// Reconnect backoff
// ---------------------------------------------------------------------------

void test_reconnect_backoff(void) {
    ReconnectBackoff b;
    b.minDelayMs = 1000;
    b.maxDelayMs = 5000;
    b.reset(100);
    TEST_ASSERT_TRUE(b.due(100));

    b.failed(100);                  // next at 1100, then 2000
    TEST_ASSERT_FALSE(b.due(1099));
    TEST_ASSERT_TRUE(b.due(1100));

    b.failed(1100);                 // next at 3100, then 4000
    TEST_ASSERT_FALSE(b.due(3099));
    TEST_ASSERT_TRUE(b.due(3100));

    b.failed(3100);                 // next at 7100, then capped at 5000
    TEST_ASSERT_TRUE(b.due(7100));
    b.failed(7100);                 // next at 12100
    TEST_ASSERT_FALSE(b.due(12099));
    TEST_ASSERT_TRUE(b.due(12100));

    b.reset(20000);
    TEST_ASSERT_TRUE(b.due(20000));
    TEST_ASSERT_EQUAL(1000, b.currentDelayMs);

    b.enabled = false;
    TEST_ASSERT_FALSE(b.due(20000));
}

void test_reconnect_backoff_wraps(void) {
    ReconnectBackoff b;
    b.reset(0xFFFFFF00u);
    b.failed(0xFFFFFF00u);          // next attempt wraps past 0
    TEST_ASSERT_FALSE(b.due(0xFFFFFFFFu));
    TEST_ASSERT_TRUE(b.due(0xFFFFFF00u + 1000u));
}

static void runAll() {
    RUN_TEST(test_sensor_component);
    RUN_TEST(test_switch_component);
    RUN_TEST(test_binary_sensor_component);
    RUN_TEST(test_button_component);
    RUN_TEST(test_entities_share_one_payload);
    RUN_TEST(test_changed_entity_republishes);
    RUN_TEST(test_no_payload_without_entities);
    RUN_TEST(test_origin_defaults_to_library);
    RUN_TEST(test_set_origin);
    RUN_TEST(test_default_entity_id);
    RUN_TEST(test_use_device_name_sends_null_name);
    RUN_TEST(test_icon_uses_abbreviation);
    RUN_TEST(test_availability_override_in_component);
    RUN_TEST(test_device_strings_are_copied);
    RUN_TEST(test_no_device_publishes_nothing);
    RUN_TEST(test_publish_state);
    RUN_TEST(test_publish_state_switch);
    RUN_TEST(test_state_waits_for_device_payload);
    RUN_TEST(test_availability);
    RUN_TEST(test_will_set_by_set_device);
    RUN_TEST(test_switch_command_routed_in_tick);
    RUN_TEST(test_button_command_override_topic);
    RUN_TEST(test_unknown_topic_ignored);
    RUN_TEST(test_incoming_queue_is_bounded);
    RUN_TEST(test_subscribe_waits_for_connect);
    RUN_TEST(test_removed_switch_stops_commands);
    RUN_TEST(test_publish_while_disconnected_is_deferred);
    RUN_TEST(test_reconnect_republishes_last_state);
    RUN_TEST(test_publish_during_pending_connect_sent_once);
    RUN_TEST(test_birth_message_republishes_on_tick);
    RUN_TEST(test_birth_message_waits_for_delay);
    RUN_TEST(test_birth_offline_ignored);
    RUN_TEST(test_remove_entity_sends_stub_then_omits);
    RUN_TEST(test_remove_last_entity_clears_device_topic);
    RUN_TEST(test_readd_before_tick_cancels_removal);
    RUN_TEST(test_remove_device);
    RUN_TEST(test_remove_device_while_disconnected);
    RUN_TEST(test_migration_from_legacy_topics);
    RUN_TEST(test_migration_waits_before_clearing);
    RUN_TEST(test_reconnect_backoff);
    RUN_TEST(test_reconnect_backoff_wraps);
}

// Support for native environment where setup/loop might not be enough for unity runner
#if defined(ARDUINO)
void setup() {
    delay(2000);
    UNITY_BEGIN();
    runAll();
    UNITY_END();
}

void loop() {}
#else
int main(int argc, char **argv) {
    UNITY_BEGIN();
    runAll();
    return UNITY_END();
}
#endif
