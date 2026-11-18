#include <unity.h>
#include <string>
#include <vector>
#include <cstring>
#include <chrono>
#include <thread>
#include "HaDiscovery.h"
#include "transport/MqttTransport.h"
#include "transport/MessageAssembler.h"
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

    // Refuse publishes, like a client short of memory or with a full outbox
    bool rejectAll = false;
    std::vector<std::string> rejectTopics;
    int rejected = 0;

    bool publish(const char* topic, const uint8_t* payload, size_t len, bool retained, uint8_t qos) override {
        bool reject = rejectAll;
        for (const std::string& t : rejectTopics) {
            reject = reject || t == topic;
        }
        if (reject) {
            rejected++;
            return false;
        }
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

    // Expose the protected request flag for tests
    bool takeRequest() { return takeReconnectRequest(); }

    void clear() {
        messages.clear();
        subscriptions.clear();
        willTopic.clear();
        willPayload.clear();
        isConnected = true;
        rejectAll = false;
        rejectTopics.clear();
        rejected = 0;
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
    dev.configuration_url = "http://test.local/";
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
    TEST_ASSERT_EQUAL_STRING("http://test.local/", doc["dev"]["cu"]);
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
    TEST_ASSERT_FALSE(doc["dev"]["cu"].is<const char*>());  // not set: left out
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
// Removal
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


// ---------------------------------------------------------------------------
// entity_category, expire_after
// ---------------------------------------------------------------------------

void test_entity_category(void) {
    HaSensorConfig rssi;
    rssi.common.object_id = "rssi";
    rssi.common.entity_category = HaEntityCategory::Diagnostic;
    discovery->publishSensorDiscovery(rssi);

    HaSensorConfig bad;
    bad.common.object_id = "bad";
    bad.common.entity_category = HaEntityCategory::Config;  // not allowed on a sensor
    discovery->publishSensorDiscovery(bad);

    HaNumberConfig interval;
    interval.common.object_id = "interval";
    interval.common.entity_category = HaEntityCategory::Config;
    discovery->publishNumberDiscovery(interval);

    addSensor("plain");
    discovery->tick();

    JsonDocument doc = lastDevicePayload();
    JsonObject cmps = doc["cmps"];
    TEST_ASSERT_EQUAL_STRING("diagnostic", cmps["rssi"]["ent_cat"]);
    TEST_ASSERT_FALSE(cmps["bad"]["ent_cat"].is<const char*>());
    TEST_ASSERT_EQUAL_STRING("config", cmps["interval"]["ent_cat"]);
    TEST_ASSERT_FALSE(cmps["plain"]["ent_cat"].is<const char*>());
}

void test_expire_after(void) {
    HaSensorConfig s1;
    s1.common.object_id = "temp";
    s1.expire_after = 300;
    discovery->publishSensorDiscovery(s1);

    HaBinarySensorConfig b1;
    b1.common.object_id = "motion";
    b1.expire_after = 60;
    discovery->publishBinarySensorDiscovery(b1);

    addSensor("plain");
    discovery->tick();

    JsonDocument doc = lastDevicePayload();
    JsonObject cmps = doc["cmps"];
    TEST_ASSERT_EQUAL(300, cmps["temp"]["exp_aft"].as<int>());
    TEST_ASSERT_EQUAL(60, cmps["motion"]["exp_aft"].as<int>());
    TEST_ASSERT_FALSE(cmps["plain"]["exp_aft"].is<int>());
}

// ---------------------------------------------------------------------------
// number, select, text
// ---------------------------------------------------------------------------

void test_number_component_and_command(void) {
    discovery->onCommand(recordCommand);
    HaNumberConfig cfg;
    cfg.common.object_id = "interval";
    cfg.min = 5;
    cfg.max = 60;
    cfg.step = 0.5f;
    cfg.mode = HaNumberMode::Slider;
    cfg.unit_of_measurement = "s";
    TEST_ASSERT_TRUE(discovery->publishNumberDiscovery(cfg));
    discovery->tick();

    JsonDocument doc = lastDevicePayload();
    JsonObject c = doc["cmps"]["interval"];
    TEST_ASSERT_EQUAL_STRING("number", c["p"]);
    TEST_ASSERT_EQUAL_FLOAT(5, c["min"].as<float>());
    TEST_ASSERT_EQUAL_FLOAT(60, c["max"].as<float>());
    TEST_ASSERT_EQUAL_FLOAT(0.5f, c["step"].as<float>());
    TEST_ASSERT_EQUAL_STRING("slider", c["mode"]);
    TEST_ASSERT_EQUAL_STRING("s", c["unit_of_meas"]);
    TEST_ASSERT_EQUAL_STRING("devices/test_node/interval/set", c["cmd_t"]);
    TEST_ASSERT_EQUAL_STRING("devices/test_node/interval/state", c["stat_t"]);

    transport.deliver("devices/test_node/interval/set", "12.5");
    discovery->tick();
    TEST_ASSERT_EQUAL(1, commands.size());
    TEST_ASSERT_EQUAL_STRING("12.5", commands[0].second.c_str());

    transport.clear();
    discovery->publishStateNumber("interval", 12.5, 1);
    TEST_ASSERT_EQUAL_STRING("12.5", transport.messages[0].payload.c_str());
    transport.clear();
    discovery->publishStateNumber("interval", 7, 0);
    TEST_ASSERT_EQUAL_STRING("7", transport.messages[0].payload.c_str());
}

void test_number_rejects_bad_range(void) {
    HaNumberConfig cfg;
    cfg.common.object_id = "bad";
    cfg.min = 10;
    cfg.max = 5;
    TEST_ASSERT_FALSE(discovery->publishNumberDiscovery(cfg));
    cfg.min = 0;
    cfg.step = 0;
    TEST_ASSERT_FALSE(discovery->publishNumberDiscovery(cfg));
}

void test_select_component(void) {
    discovery->onCommand(recordCommand);
    HaSelectConfig cfg;
    cfg.common.object_id = "mode";
    cfg.options = {"Eco", "Normal", "Turbo"};
    TEST_ASSERT_TRUE(discovery->publishSelectDiscovery(cfg));
    discovery->tick();

    JsonDocument doc = lastDevicePayload();
    JsonObject c = doc["cmps"]["mode"];
    TEST_ASSERT_EQUAL_STRING("select", c["p"]);
    TEST_ASSERT_EQUAL(3, c["ops"].as<JsonArray>().size());
    TEST_ASSERT_EQUAL_STRING("Turbo", c["ops"][2]);

    transport.deliver("devices/test_node/mode/set", "Eco");
    discovery->tick();
    TEST_ASSERT_EQUAL_STRING("Eco", commands[0].second.c_str());

    HaSelectConfig empty;
    empty.common.object_id = "empty";
    TEST_ASSERT_FALSE(discovery->publishSelectDiscovery(empty));
}

void test_text_component(void) {
    HaTextConfig cfg;
    cfg.common.object_id = "message";
    cfg.max_length = 32;
    cfg.mode = HaTextMode::Password;
    cfg.pattern = "[a-z]*";
    TEST_ASSERT_TRUE(discovery->publishTextDiscovery(cfg));

    HaTextConfig plain;
    plain.common.object_id = "plain";
    discovery->publishTextDiscovery(plain);
    discovery->tick();

    JsonDocument doc = lastDevicePayload();
    JsonObject c = doc["cmps"]["message"];
    TEST_ASSERT_EQUAL_STRING("text", c["p"]);
    TEST_ASSERT_EQUAL(32, c["max"].as<int>());
    TEST_ASSERT_EQUAL_STRING("password", c["mode"]);
    TEST_ASSERT_EQUAL_STRING("[a-z]*", c["ptrn"]);
    JsonObject p = doc["cmps"]["plain"];
    TEST_ASSERT_FALSE(p["min"].is<int>());
    TEST_ASSERT_FALSE(p["max"].is<int>());
    TEST_ASSERT_FALSE(p["mode"].is<const char*>());

    HaTextConfig bad;
    bad.common.object_id = "bad";
    bad.min_length = 10;
    bad.max_length = 5;
    TEST_ASSERT_FALSE(discovery->publishTextDiscovery(bad));
}

// ---------------------------------------------------------------------------
// light
// ---------------------------------------------------------------------------

void test_light_components(void) {
    HaLightConfig rgb;
    rgb.common.object_id = "rgb";
    rgb.color_mode = HaLightColorMode::Rgb;
    discovery->publishLightDiscovery(rgb);

    HaLightConfig ct;
    ct.common.object_id = "ct";
    ct.color_mode = HaLightColorMode::ColorTemp;
    ct.min_kelvin = 2700;
    ct.max_kelvin = 6000;
    ct.brightness_scale = 100;
    discovery->publishLightDiscovery(ct);

    HaLightConfig onoff;
    onoff.common.object_id = "onoff";
    onoff.color_mode = HaLightColorMode::OnOff;
    discovery->publishLightDiscovery(onoff);
    discovery->tick();

    JsonDocument doc = lastDevicePayload();
    JsonObject cmps = doc["cmps"];
    TEST_ASSERT_EQUAL_STRING("light", cmps["rgb"]["p"]);
    TEST_ASSERT_EQUAL_STRING("json", cmps["rgb"]["schema"]);
    TEST_ASSERT_EQUAL_STRING("rgb", cmps["rgb"]["supported_color_modes"][0]);
    TEST_ASSERT_EQUAL_STRING("devices/test_node/rgb/set", cmps["rgb"]["cmd_t"]);
    TEST_ASSERT_FALSE(cmps["rgb"]["brightness_scale"].is<int>());
    TEST_ASSERT_TRUE(cmps["rgb"]["brightness"].as<bool>());  // brightness sent separately from color

    TEST_ASSERT_EQUAL_STRING("color_temp", cmps["ct"]["supported_color_modes"][0]);
    TEST_ASSERT_TRUE(cmps["ct"]["color_temp_kelvin"].as<bool>());
    TEST_ASSERT_EQUAL(2700, cmps["ct"]["min_kelvin"].as<int>());
    TEST_ASSERT_EQUAL(6000, cmps["ct"]["max_kelvin"].as<int>());
    TEST_ASSERT_EQUAL(100, cmps["ct"]["brightness_scale"].as<int>());

    TEST_ASSERT_EQUAL_STRING("onoff", cmps["onoff"]["supported_color_modes"][0]);
    TEST_ASSERT_FALSE(cmps["onoff"]["brightness"].is<bool>());
}

void test_parse_light_command(void) {
    HaLightCommand cmd;
    TEST_ASSERT_TRUE(HaDiscovery::parseLightCommand(
        "{\"state\":\"ON\",\"brightness\":128,\"color\":{\"r\":255,\"g\":10,\"b\":0},\"transition\":2.5}", cmd));
    TEST_ASSERT_TRUE(cmd.has_state);
    TEST_ASSERT_TRUE(cmd.on);
    TEST_ASSERT_TRUE(cmd.has_brightness);
    TEST_ASSERT_EQUAL(128, cmd.brightness);
    TEST_ASSERT_TRUE(cmd.has_color);
    TEST_ASSERT_EQUAL(255, cmd.r);
    TEST_ASSERT_EQUAL(10, cmd.g);
    TEST_ASSERT_TRUE(cmd.has_transition);
    TEST_ASSERT_EQUAL_FLOAT(2.5f, cmd.transition);
    TEST_ASSERT_FALSE(cmd.has_color_temp);

    TEST_ASSERT_TRUE(HaDiscovery::parseLightCommand("{\"state\":\"OFF\"}", cmd));
    TEST_ASSERT_TRUE(cmd.has_state);
    TEST_ASSERT_FALSE(cmd.on);
    TEST_ASSERT_FALSE(cmd.has_brightness);

    TEST_ASSERT_TRUE(HaDiscovery::parseLightCommand("{\"color_temp\":3000}", cmd));
    TEST_ASSERT_FALSE(cmd.has_state);
    TEST_ASSERT_TRUE(cmd.has_color_temp);
    TEST_ASSERT_EQUAL(3000, cmd.color_temp_kelvin);

    TEST_ASSERT_FALSE(HaDiscovery::parseLightCommand("ON", cmd));
    TEST_ASSERT_FALSE(HaDiscovery::parseLightCommand(nullptr, cmd));
}

void test_publish_state_light(void) {
    JsonDocument doc;
    HaLightState st;
    st.on = true;
    st.brightness = 200;
    discovery->publishStateLight("lamp", st);
    TEST_ASSERT_EQUAL_STRING("devices/test_node/lamp/state", transport.messages[0].topic.c_str());
    deserializeJson(doc, transport.messages[0].payload);
    TEST_ASSERT_EQUAL_STRING("ON", doc["state"]);
    TEST_ASSERT_EQUAL(200, doc["brightness"].as<int>());
    TEST_ASSERT_EQUAL_STRING("brightness", doc["color_mode"]);

    st.r = 1; st.g = 2; st.b = 3;
    discovery->publishStateLight("lamp", st);
    deserializeJson(doc, transport.messages[1].payload);
    TEST_ASSERT_EQUAL_STRING("rgb", doc["color_mode"]);
    TEST_ASSERT_EQUAL(3, doc["color"]["b"].as<int>());

    HaLightState ct;
    ct.on = true;
    ct.color_temp_kelvin = 4000;
    discovery->publishStateLight("lamp", ct);
    deserializeJson(doc, transport.messages[2].payload);
    TEST_ASSERT_EQUAL_STRING("color_temp", doc["color_mode"]);
    TEST_ASSERT_EQUAL(4000, doc["color_temp"].as<int>());

    HaLightState off;
    discovery->publishStateLight("lamp", off);
    deserializeJson(doc, transport.messages[3].payload);
    TEST_ASSERT_EQUAL_STRING("OFF", doc["state"]);
    TEST_ASSERT_EQUAL_STRING("onoff", doc["color_mode"]);
}

// ---------------------------------------------------------------------------
// cover
// ---------------------------------------------------------------------------

void test_cover_component_and_commands(void) {
    discovery->onCommand(recordCommand);
    HaCoverConfig cfg;
    cfg.common.object_id = "blind";
    cfg.device_class = "blind";
    cfg.has_position = true;
    TEST_ASSERT_TRUE(discovery->publishCoverDiscovery(cfg));

    HaCoverConfig door;
    door.common.object_id = "door";
    door.payload_stop = "";  // can't stop
    discovery->publishCoverDiscovery(door);
    discovery->tick();

    JsonDocument doc = lastDevicePayload();
    JsonObject c = doc["cmps"]["blind"];
    TEST_ASSERT_EQUAL_STRING("cover", c["p"]);
    TEST_ASSERT_EQUAL_STRING("blind", c["dev_cla"]);
    TEST_ASSERT_EQUAL_STRING("devices/test_node/blind/set", c["cmd_t"]);
    TEST_ASSERT_EQUAL_STRING("devices/test_node/blind/position", c["pos_t"]);
    TEST_ASSERT_EQUAL_STRING("devices/test_node/blind/set_position", c["set_pos_t"]);
    TEST_ASSERT_TRUE(doc["cmps"]["door"]["pl_stop"].isNull());
    TEST_ASSERT_FALSE(doc["cmps"]["door"]["pos_t"].is<const char*>());

    transport.deliver("devices/test_node/blind/set", "OPEN");
    transport.deliver("devices/test_node/blind/set_position", "40");
    discovery->tick();
    TEST_ASSERT_EQUAL(2, commands.size());
    TEST_ASSERT_EQUAL_STRING("blind", commands[0].first.c_str());
    TEST_ASSERT_EQUAL_STRING("OPEN", commands[0].second.c_str());
    TEST_ASSERT_EQUAL_STRING("blind", commands[1].first.c_str());
    TEST_ASSERT_EQUAL_STRING("40", commands[1].second.c_str());

    transport.clear();
    discovery->publishStatePosition("blind", 140);
    TEST_ASSERT_EQUAL_STRING("devices/test_node/blind/position", transport.messages[0].topic.c_str());
    TEST_ASSERT_EQUAL_STRING("100", transport.messages[0].payload.c_str());
}

void test_cover_removed_drops_both_routes_and_states(void) {
    discovery->onCommand(recordCommand);
    HaCoverConfig cfg;
    cfg.common.object_id = "blind";
    cfg.has_position = true;
    discovery->publishCoverDiscovery(cfg);
    discovery->tick();
    discovery->publishState("blind", "open");
    discovery->publishStatePosition("blind", 100);

    discovery->removeEntity("cover", "blind");
    transport.deliver("devices/test_node/blind/set", "CLOSE");
    transport.deliver("devices/test_node/blind/set_position", "0");
    discovery->tick();
    TEST_ASSERT_EQUAL(0, commands.size());

    transport.clear();
    transport.fireConnect();
    discovery->tick();
    TEST_ASSERT_EQUAL(-1, transport.indexOf("devices/test_node/blind/state"));
    TEST_ASSERT_EQUAL(-1, transport.indexOf("devices/test_node/blind/position"));
}

void test_republish_cover_without_position_drops_route(void) {
    discovery->onCommand(recordCommand);
    HaCoverConfig cfg;
    cfg.common.object_id = "blind";
    cfg.has_position = true;
    discovery->publishCoverDiscovery(cfg);
    cfg.has_position = false;
    discovery->publishCoverDiscovery(cfg);

    transport.deliver("devices/test_node/blind/set_position", "10");
    transport.deliver("devices/test_node/blind/set", "STOP");
    discovery->tick();
    TEST_ASSERT_EQUAL(1, commands.size());
    TEST_ASSERT_EQUAL_STRING("STOP", commands[0].second.c_str());
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

void test_reconnect_now_sets_request_once(void) {
    TEST_ASSERT_FALSE(transport.takeRequest());
    transport.reconnectNow();
    TEST_ASSERT_TRUE(transport.takeRequest());
    TEST_ASSERT_FALSE(transport.takeRequest());
}

void test_reconnect_backoff_wraps(void) {
    ReconnectBackoff b;
    b.reset(0xFFFFFF00u);
    b.failed(0xFFFFFF00u);          // next attempt wraps past 0
    TEST_ASSERT_FALSE(b.due(0xFFFFFFFFu));
    TEST_ASSERT_TRUE(b.due(0xFFFFFF00u + 1000u));
}


// ---------------------------------------------------------------------------
// 1.1.0: value templates, unique_id, state topics, default schema lights,
// raw messages, availability topic, migration
// ---------------------------------------------------------------------------

static std::vector<std::pair<std::string, std::string>> rawMessages;

static void recordMessage(const char* topic, const char* payload) {
    rawMessages.push_back({topic, payload});
}

static void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static const char* kSyncTopic = "devices/test_node/migration_sync";

// The broker sends our migration sync message back, after the retained configs
static void deliverMigrationSync() {
    std::vector<MockTransport::Message> sync = transport.on(kSyncTopic);
    TEST_ASSERT_FALSE(sync.empty());
    transport.deliver(kSyncTopic, sync.back().payload.c_str());
}

void test_value_template_and_unique_id(void) {
    HaSensorConfig ip;
    ip.common.object_id = "ip";
    ip.common.state_topic_override = "wopr/test_node/diagnostic/state";
    ip.common.value_template = "{{ value_json.ipAddress }}";
    ip.common.unique_id = "old_unique_ip";
    discovery->publishSensorDiscovery(ip);

    HaButtonConfig restart;
    restart.common.object_id = "restart";
    restart.common.value_template = "{{ value }}";  // no state, left out
    discovery->publishButtonDiscovery(restart);
    discovery->tick();

    JsonDocument doc = lastDevicePayload();
    TEST_ASSERT_EQUAL_STRING("{{ value_json.ipAddress }}", doc["cmps"]["ip"]["val_tpl"]);
    TEST_ASSERT_EQUAL_STRING("old_unique_ip", doc["cmps"]["ip"]["uniq_id"]);
    TEST_ASSERT_EQUAL_STRING("wopr/test_node/diagnostic/state", doc["cmps"]["ip"]["stat_t"]);
    TEST_ASSERT_FALSE(doc["cmps"]["restart"]["val_tpl"].is<const char*>());
    TEST_ASSERT_EQUAL_STRING("test_node_restart", doc["cmps"]["restart"]["uniq_id"]);
}

void test_publish_state_uses_state_topic_override(void) {
    HaSwitchConfig sw;
    sw.common.object_id = "relay";
    sw.common.state_topic_override = "custom/relay/state";
    discovery->publishSwitchDiscovery(sw);
    discovery->tick();
    transport.messages.clear();

    discovery->publishStateSwitch("relay", true);
    TEST_ASSERT_EQUAL(1, transport.on("custom/relay/state").size());
    TEST_ASSERT_EQUAL(0, transport.on("devices/test_node/relay/state").size());

    // Not an entity: the default topic
    discovery->publishState("other", "x");
    TEST_ASSERT_EQUAL(1, transport.on("devices/test_node/other/state").size());
}

void test_publish_state_topic_is_republished(void) {
    addSensor("ip");
    discovery->tick();
    transport.messages.clear();

    TEST_ASSERT_TRUE(discovery->publishStateTopic("wopr/test_node/diagnostic/state", "{\"ip\":\"1\"}"));
    TEST_ASSERT_TRUE(discovery->publishStateTopic("wopr/test_node/diagnostic/state", "{\"ip\":\"2\"}"));
    TEST_ASSERT_EQUAL(2, transport.on("wopr/test_node/diagnostic/state").size());

    transport.messages.clear();
    transport.fireConnect();
    discovery->tick();
    std::vector<MockTransport::Message> msgs = transport.on("wopr/test_node/diagnostic/state");
    TEST_ASSERT_EQUAL(1, msgs.size());
    TEST_ASSERT_EQUAL_STRING("{\"ip\":\"2\"}", msgs[0].payload.c_str());
    TEST_ASSERT_TRUE(transport.indexOf(kDeviceTopic) < transport.indexOf("wopr/test_node/diagnostic/state"));
    TEST_ASSERT_FALSE(discovery->publishStateTopic(nullptr, "x"));
}

void test_default_schema_light_config(void) {
    HaLightConfig rgb;
    rgb.common.object_id = "defcon";
    rgb.schema = HaLightSchema::Default;
    rgb.color_mode = HaLightColorMode::Rgb;
    rgb.brightness_scale = 100;
    rgb.common.state_topic_override = "wopr/test_node/defcon/state";
    rgb.command_topic_override = "wopr/test_node/defcon/state/set";
    rgb.rgb_state_topic_override = "wopr/test_node/defcon/color";
    rgb.rgb_command_topic_override = "wopr/test_node/defcon/color/set";
    rgb.common.value_template = "{{ value }}";
    discovery->publishLightDiscovery(rgb);

    HaLightConfig ct;
    ct.common.object_id = "ct";
    ct.schema = HaLightSchema::Default;
    ct.color_mode = HaLightColorMode::ColorTemp;
    discovery->publishLightDiscovery(ct);

    HaLightConfig onoff;
    onoff.common.object_id = "onoff";
    onoff.schema = HaLightSchema::Default;
    onoff.color_mode = HaLightColorMode::OnOff;
    discovery->publishLightDiscovery(onoff);
    discovery->tick();

    JsonDocument doc = lastDevicePayload();
    JsonObject d = doc["cmps"]["defcon"];
    TEST_ASSERT_FALSE(d["schema"].is<const char*>());
    TEST_ASSERT_FALSE(d["supported_color_modes"].is<JsonArray>());
    TEST_ASSERT_EQUAL_STRING("wopr/test_node/defcon/state", d["stat_t"]);
    TEST_ASSERT_EQUAL_STRING("wopr/test_node/defcon/state/set", d["cmd_t"]);
    TEST_ASSERT_EQUAL_STRING("{{ value }}", d["stat_val_tpl"]);
    TEST_ASSERT_EQUAL_STRING("devices/test_node/defcon/brightness", d["brightness_state_topic"]);
    TEST_ASSERT_EQUAL_STRING("devices/test_node/defcon/brightness/set", d["brightness_command_topic"]);
    TEST_ASSERT_EQUAL_STRING("wopr/test_node/defcon/color", d["rgb_state_topic"]);
    TEST_ASSERT_EQUAL_STRING("wopr/test_node/defcon/color/set", d["rgb_command_topic"]);
    TEST_ASSERT_EQUAL(100, d["brightness_scale"].as<int>());

    JsonObject c = doc["cmps"]["ct"];
    TEST_ASSERT_EQUAL_STRING("devices/test_node/ct/color_temp/set", c["color_temp_command_topic"]);
    TEST_ASSERT_TRUE(c["color_temp_kelvin"].as<bool>());
    TEST_ASSERT_FALSE(c["rgb_command_topic"].is<const char*>());

    JsonObject o = doc["cmps"]["onoff"];
    TEST_ASSERT_FALSE(o["brightness_command_topic"].is<const char*>());

    // Subscribed to every command topic
    transport.subscriptions.clear();
    transport.fireConnect();
    discovery->tick();
    auto subscribed = [](const char* t) {
        for (const std::string& s : transport.subscriptions) {
            if (s == t) return true;
        }
        return false;
    };
    TEST_ASSERT_TRUE(subscribed("wopr/test_node/defcon/state/set"));
    TEST_ASSERT_TRUE(subscribed("devices/test_node/defcon/brightness/set"));
    TEST_ASSERT_TRUE(subscribed("wopr/test_node/defcon/color/set"));
    TEST_ASSERT_TRUE(subscribed("devices/test_node/ct/color_temp/set"));
}

void test_default_schema_light_commands_become_json(void) {
    HaLightConfig rgb;
    rgb.common.object_id = "lamp";
    rgb.schema = HaLightSchema::Default;
    rgb.color_mode = HaLightColorMode::Rgb;
    discovery->publishLightDiscovery(rgb);
    discovery->onCommand(recordCommand);
    discovery->tick();

    transport.deliver("devices/test_node/lamp/set", "ON");
    transport.deliver("devices/test_node/lamp/brightness/set", "128");
    transport.deliver("devices/test_node/lamp/rgb/set", "255,300,-1");
    transport.deliver("devices/test_node/lamp/rgb/set", "garbage");
    discovery->tick();

    TEST_ASSERT_EQUAL(3, commands.size());
    HaLightCommand cmd;
    TEST_ASSERT_TRUE(HaDiscovery::parseLightCommand(commands[0].second.c_str(), cmd));
    TEST_ASSERT_TRUE(cmd.has_state);
    TEST_ASSERT_TRUE(cmd.on);
    TEST_ASSERT_TRUE(HaDiscovery::parseLightCommand(commands[1].second.c_str(), cmd));
    TEST_ASSERT_TRUE(cmd.has_brightness);
    TEST_ASSERT_EQUAL(128, cmd.brightness);
    TEST_ASSERT_TRUE(HaDiscovery::parseLightCommand(commands[2].second.c_str(), cmd));
    TEST_ASSERT_TRUE(cmd.has_color);
    TEST_ASSERT_EQUAL(255, cmd.r);
    TEST_ASSERT_EQUAL(255, cmd.g);
    TEST_ASSERT_EQUAL(0, cmd.b);
    TEST_ASSERT_EQUAL_STRING("lamp", commands[2].first.c_str());
}

void test_default_schema_light_state(void) {
    HaLightConfig rgb;
    rgb.common.object_id = "lamp";
    rgb.schema = HaLightSchema::Default;
    rgb.color_mode = HaLightColorMode::Rgb;
    discovery->publishLightDiscovery(rgb);
    discovery->tick();
    transport.messages.clear();

    HaLightState st;
    st.on = true;
    st.brightness = 40;
    st.r = 1; st.g = 2; st.b = 3;
    st.color_temp_kelvin = 3000;  // the light has no color temperature
    TEST_ASSERT_TRUE(discovery->publishStateLight("lamp", st));
    TEST_ASSERT_EQUAL_STRING("ON", transport.on("devices/test_node/lamp/state")[0].payload.c_str());
    TEST_ASSERT_EQUAL_STRING("40", transport.on("devices/test_node/lamp/brightness")[0].payload.c_str());
    TEST_ASSERT_EQUAL_STRING("1,2,3", transport.on("devices/test_node/lamp/rgb")[0].payload.c_str());
    TEST_ASSERT_EQUAL(0, transport.on("devices/test_node/lamp/color_temp").size());
    TEST_ASSERT_EQUAL(3, transport.messages.size());

    // Removing it drops its stored states
    discovery->removeEntity("light", "lamp");
    discovery->tick();
    transport.messages.clear();
    transport.fireConnect();
    discovery->tick();
    TEST_ASSERT_EQUAL(0, transport.on("devices/test_node/lamp/rgb").size());
}

void test_on_message_and_subscribe(void) {
    HaSwitchConfig sw;
    sw.common.object_id = "relay";
    discovery->publishSwitchDiscovery(sw);
    discovery->subscribe("devices/test_node/extra/#", 1);
    discovery->onMessage(recordMessage);
    discovery->onCommand(recordCommand);
    rawMessages.clear();

    transport.subscriptions.clear();
    transport.fireConnect();
    discovery->tick();
    bool found = false;
    for (const std::string& t : transport.subscriptions) {
        found = found || t == "devices/test_node/extra/#";
    }
    TEST_ASSERT_TRUE(found);

    // Added while connected: subscribed right away
    transport.subscriptions.clear();
    discovery->subscribe("other/topic");
    TEST_ASSERT_EQUAL(1, transport.subscriptions.size());

    transport.deliver("devices/test_node/relay/set", "ON");
    transport.deliver("devices/test_node/extra/thing", "hello");
    transport.deliver("homeassistant/status", "online");
    discovery->tick();

    TEST_ASSERT_EQUAL(2, rawMessages.size());
    TEST_ASSERT_EQUAL_STRING("devices/test_node/relay/set", rawMessages[0].first.c_str());
    TEST_ASSERT_EQUAL_STRING("ON", rawMessages[0].second.c_str());
    TEST_ASSERT_EQUAL_STRING("devices/test_node/extra/thing", rawMessages[1].first.c_str());
    TEST_ASSERT_EQUAL(1, commands.size());  // onCommand still gets entity commands
}

void test_set_availability_topic(void) {
    discovery->setAvailabilityTopic("wopr/test_node/availability");
    TEST_ASSERT_EQUAL_STRING("wopr/test_node/availability", transport.willTopic.c_str());
    TEST_ASSERT_EQUAL_STRING("offline", transport.willPayload.c_str());

    addSensor();
    discovery->tick();
    JsonDocument doc = lastDevicePayload();
    TEST_ASSERT_EQUAL_STRING("wopr/test_node/availability", doc["avty_t"]);
    TEST_ASSERT_EQUAL(1, transport.on("wopr/test_node/availability").size());
    TEST_ASSERT_EQUAL(0, transport.on("devices/test_node/status").size());

    // Back to the default
    discovery->setAvailabilityTopic(nullptr);
    TEST_ASSERT_EQUAL_STRING("devices/test_node/status", transport.willTopic.c_str());
}

void test_migrate_entity_discovery(void) {
    const char* oldTopic = "homeassistant/sensor/test_node/diagnostic_ip/config";
    discovery->migrateEntityDiscovery();
    HaSensorConfig ip;
    ip.common.object_id = "ip";
    ip.common.unique_id = "test_node_diagnostic_ip";
    discovery->publishSensorDiscovery(ip);
    discovery->publishState("ip", "1.2.3.4");

    transport.messages.clear();
    transport.subscriptions.clear();
    transport.fireConnect();
    discovery->tick();
    bool found = false;
    for (const std::string& t : transport.subscriptions) {
        found = found || t == "homeassistant/+/test_node/+/config";
    }
    TEST_ASSERT_TRUE(found);
    // Waits for retained single-component configs first
    TEST_ASSERT_EQUAL(0, transport.on(kDeviceTopic).size());
    TEST_ASSERT_EQUAL(0, transport.on("devices/test_node/ip/state").size());

    // The broker sends the retained old config (and other topics that aren't ours)
    transport.deliver(oldTopic, "{\"name\":\"IP\",\"uniq_id\":\"test_node_diagnostic_ip\"}");
    transport.deliver("homeassistant/sensor/other_node/x/config", "{\"name\":\"X\"}");
    transport.deliver("homeassistant/sensor/test_node/cleared/config", "");
    discovery->tick();
    std::vector<MockTransport::Message> old = transport.on(oldTopic);
    TEST_ASSERT_EQUAL(1, old.size());
    TEST_ASSERT_TRUE(old[0].retained);
    TEST_ASSERT_EQUAL(0, old[0].qos);  // QoS 0, so AsyncMqttClient doesn't wait for a PUBACK per message
    JsonDocument m;
    deserializeJson(m, old[0].payload);
    TEST_ASSERT_TRUE(m["migrate_discovery"].as<bool>());
    TEST_ASSERT_EQUAL(0, transport.on(kDeviceTopic).size());
    TEST_ASSERT_EQUAL(0, transport.on("homeassistant/sensor/test_node/cleared/config").size());

    // Our own migrate message comes back; nothing new is published for it
    transport.deliver(oldTopic, old[0].payload.c_str());
    discovery->tick();
    TEST_ASSERT_EQUAL(1, transport.on(oldTopic).size());

    // Once the sync message is back: device payload, then the states
    deliverMigrationSync();
    discovery->tick();
    TEST_ASSERT_EQUAL(1, transport.on(kDeviceTopic).size());
    TEST_ASSERT_TRUE(transport.indexOf(oldTopic) < transport.indexOf(kDeviceTopic));
    TEST_ASSERT_TRUE(transport.indexOf(kDeviceTopic) < transport.indexOf("devices/test_node/ip/state"));
    TEST_ASSERT_EQUAL(1, transport.on(kDeviceTopic)[0].qos);  // the device payload stays at QoS 1
    TEST_ASSERT_EQUAL_STRING("test_node_diagnostic_ip", lastDevicePayload()["cmps"]["ip"]["uniq_id"]);
    TEST_ASSERT_EQUAL(1, transport.on(oldTopic).size());  // not cleared yet

    // 5 s after the migrate message the old topic is cleared
    sleepMs(5100);
    discovery->tick();
    old = transport.on(oldTopic);
    TEST_ASSERT_EQUAL(2, old.size());
    TEST_ASSERT_TRUE(old[1].payload.empty());
    TEST_ASSERT_TRUE(old[1].retained);
    TEST_ASSERT_EQUAL(0, old[1].qos);

    // Without migration, messages on those topics are just passed on
    discovery->migrateEntityDiscovery(false);
    discovery->onMessage(recordMessage);
    rawMessages.clear();
    transport.deliver(oldTopic, "{\"name\":\"IP\"}");
    discovery->tick();
    TEST_ASSERT_EQUAL(1, rawMessages.size());
    TEST_ASSERT_EQUAL(2, transport.on(oldTopic).size());
}

void test_no_migration_wait_by_default(void) {
    addSensor();
    transport.messages.clear();
    transport.fireConnect();
    discovery->tick();
    TEST_ASSERT_EQUAL(1, transport.on(kDeviceTopic).size());
}

void test_migration_burst_is_not_dropped(void) {
    discovery->migrateEntityDiscovery();
    const int n = 20;  // more than the normal queue limit of 16
    for (int i = 0; i < n; i++) {
        std::string id = "s" + std::to_string(i);
        std::string uid = "old_" + id;
        HaSensorConfig cfg;
        cfg.common.object_id = id.c_str();
        cfg.common.unique_id = uid.c_str();
        discovery->publishSensorDiscovery(cfg);
    }
    transport.messages.clear();
    transport.fireConnect();
    discovery->tick();

    for (int i = 0; i < n; i++) {
        std::string topic = "homeassistant/sensor/test_node/old" + std::to_string(i) + "/config";
        std::string payload = "{\"uniq_id\":\"old_s" + std::to_string(i) + "\"}";
        transport.deliver(topic.c_str(), payload.c_str());
    }
    discovery->tick();
    for (int i = 0; i < n; i++) {
        std::string topic = "homeassistant/sensor/test_node/old" + std::to_string(i) + "/config";
        TEST_ASSERT_EQUAL_MESSAGE(1, transport.on(topic).size(), topic.c_str());
    }
}

void test_migration_skips_unknown_unique_id(void) {
    const char* oldTopic = "homeassistant/sensor/test_node/gone/config";
    discovery->migrateEntityDiscovery();
    HaSensorConfig ip;
    ip.common.object_id = "ip";
    ip.common.unique_id = "test_node_diagnostic_ip";
    discovery->publishSensorDiscovery(ip);
    transport.messages.clear();
    transport.fireConnect();
    discovery->tick();

    // unique_id mismatch, and no unique_id at all: both left alone
    transport.deliver(oldTopic, "{\"name\":\"Gone\",\"uniq_id\":\"test_node_something_else\"}");
    transport.deliver("homeassistant/sensor/test_node/noid/config", "{\"name\":\"No id\"}");
    // Full key name is accepted too
    transport.deliver("homeassistant/sensor/test_node/full/config", "{\"unique_id\":\"test_node_diagnostic_ip\"}");
    discovery->tick();
    TEST_ASSERT_EQUAL(0, transport.on(oldTopic).size());
    TEST_ASSERT_EQUAL(0, transport.on("homeassistant/sensor/test_node/noid/config").size());
    TEST_ASSERT_EQUAL(1, transport.on("homeassistant/sensor/test_node/full/config").size());

    // And they're never cleared
    deliverMigrationSync();
    discovery->tick();
    sleepMs(5100);
    discovery->tick();
    TEST_ASSERT_EQUAL(0, transport.on(oldTopic).size());
    TEST_ASSERT_EQUAL(2, transport.on("homeassistant/sensor/test_node/full/config").size());
}

void test_migration_waits_for_sync(void) {
    discovery->migrateEntityDiscovery();
    HaSensorConfig ip;
    ip.common.object_id = "ip";
    discovery->publishSensorDiscovery(ip);
    transport.messages.clear();
    transport.subscriptions.clear();
    transport.fireConnect();
    discovery->tick();

    // Subscribed to the old topics before publishing the sync message (not retained)
    size_t filterAt = transport.subscriptions.size(), syncAt = transport.subscriptions.size();
    for (size_t i = 0; i < transport.subscriptions.size(); i++) {
        if (transport.subscriptions[i] == "homeassistant/+/test_node/+/config") filterAt = i;
        if (transport.subscriptions[i] == kSyncTopic) syncAt = i;
    }
    TEST_ASSERT_TRUE(filterAt < syncAt && syncAt < transport.subscriptions.size());
    std::vector<MockTransport::Message> sync = transport.on(kSyncTopic);
    TEST_ASSERT_EQUAL(1, sync.size());
    TEST_ASSERT_FALSE(sync[0].retained);

    // A slow broker: still waiting after 2 s
    sleepMs(2100);
    discovery->tick();
    TEST_ASSERT_EQUAL(0, transport.on(kDeviceTopic).size());

    // A sync message from an earlier connection doesn't count, and isn't passed on
    discovery->onMessage(recordMessage);
    rawMessages.clear();
    transport.deliver(kSyncTopic, "stale");
    discovery->tick();
    TEST_ASSERT_EQUAL(0, transport.on(kDeviceTopic).size());
    TEST_ASSERT_EQUAL(0, rawMessages.size());

    // The old config, then our sync message: migrated first, then the device payload
    const char* oldTopic = "homeassistant/sensor/test_node/ip/config";
    transport.deliver(oldTopic, "{\"uniq_id\":\"test_node_ip\"}");
    deliverMigrationSync();
    discovery->tick();
    TEST_ASSERT_EQUAL(1, transport.on(kDeviceTopic).size());
    TEST_ASSERT_TRUE(transport.indexOf(oldTopic) < transport.indexOf(kDeviceTopic));
    TEST_ASSERT_EQUAL(0, rawMessages.size());

    // Every connect sends a new one
    transport.fireConnect();
    discovery->tick();
    sync = transport.on(kSyncTopic);
    TEST_ASSERT_EQUAL(2, sync.size());
    TEST_ASSERT_TRUE(sync[0].payload != sync[1].payload);
}

void test_migration_reads_only_what_it_needs(void) {
    discovery->migrateEntityDiscovery();
    HaSensorConfig ip;
    ip.common.object_id = "ip";
    discovery->publishSensorDiscovery(ip);
    transport.messages.clear();
    transport.fireConnect();
    discovery->tick();

    // A full old config with a device block: the unique_id is still found
    const char* full = "homeassistant/sensor/test_node/ip/config";
    transport.deliver(full, "{\"name\":\"IP\",\"stat_t\":\"x/ip\",\"dev\":{\"ids\":[\"test_node\"],\"name\":\"Old\",\"mf\":\"Me\"},"
                            "\"uniq_id\":\"test_node_ip\",\"avty\":[{\"t\":\"x/status\"}]}");
    // Not JSON: left alone
    const char* broken = "homeassistant/sensor/test_node/broken/config";
    transport.deliver(broken, "{\"uniq_id\":\"test_node_ip\"");
    discovery->tick();
    TEST_ASSERT_EQUAL(1, transport.on(full).size());
    TEST_ASSERT_EQUAL(0, transport.on(broken).size());

    deliverMigrationSync();
    discovery->tick();
    TEST_ASSERT_EQUAL(1, transport.on(kDeviceTopic).size());
}

void test_migration_wait_without_sync(void) {
    discovery->migrateEntityDiscovery();
    HaSensorConfig ip;
    ip.common.object_id = "ip";
    discovery->publishSensorDiscovery(ip);

    // The sync message can't be sent: a short fixed wait instead
    transport.messages.clear();
    transport.rejectTopics = {kSyncTopic};
    transport.fireConnect();
    discovery->tick();
    sleepMs(2100);
    discovery->tick();
    TEST_ASSERT_EQUAL(1, transport.on(kDeviceTopic).size());

    // It was sent but never comes back: published after 10 s anyway
    transport.rejectTopics.clear();
    transport.messages.clear();
    transport.fireConnect();
    discovery->tick();
    sleepMs(2100);
    discovery->tick();
    TEST_ASSERT_EQUAL(0, transport.on(kDeviceTopic).size());
    sleepMs(8000);
    discovery->tick();
    TEST_ASSERT_EQUAL(1, transport.on(kDeviceTopic).size());
}

// ---------------------------------------------------------------------------
// Publishes the transport doesn't accept
// ---------------------------------------------------------------------------

void test_device_payload_retried_after_failed_publish(void) {
    addSensor();
    discovery->publishState("temp", "21");
    transport.messages.clear();
    transport.rejectAll = true;
    transport.fireConnect();
    discovery->tick();
    TEST_ASSERT_TRUE(transport.rejected > 0);
    TEST_ASSERT_EQUAL(0, transport.messages.size());

    // Not before the retry delay, and states are held meanwhile
    transport.rejectAll = false;
    discovery->tick();
    discovery->publishState("temp", "22");
    TEST_ASSERT_EQUAL(0, transport.messages.size());

    sleepMs(1100);
    discovery->tick();
    TEST_ASSERT_EQUAL(1, transport.on(kDeviceTopic).size());
    TEST_ASSERT_EQUAL(1, transport.on("devices/test_node/status").size());
    std::vector<MockTransport::Message> st = transport.on("devices/test_node/temp/state");
    TEST_ASSERT_EQUAL(1, st.size());
    TEST_ASSERT_EQUAL_STRING("22", st[0].payload.c_str());
    TEST_ASSERT_TRUE(transport.indexOf(kDeviceTopic) < transport.indexOf("devices/test_node/temp/state"));

    // Done: nothing more
    sleepMs(1100);
    discovery->tick();
    TEST_ASSERT_EQUAL(1, transport.on(kDeviceTopic).size());
}

void test_failed_availability_is_retried(void) {
    addSensor();
    discovery->tick();
    transport.messages.clear();
    transport.rejectTopics = {"devices/test_node/status"};
    discovery->republishAll();
    TEST_ASSERT_EQUAL(0, transport.on("devices/test_node/status").size());
    transport.rejectTopics.clear();
    sleepMs(1100);
    discovery->tick();
    TEST_ASSERT_EQUAL(1, transport.on("devices/test_node/status").size());
}

void test_publish_retry_backs_off(void) {
    addSensor();
    transport.rejectAll = true;
    transport.fireConnect();
    discovery->tick();                 // attempt 1, next in 1 s
    int attempts = transport.rejected;
    TEST_ASSERT_TRUE(attempts > 0);
    sleepMs(1100);
    discovery->tick();                 // attempt 2, next in 2 s
    TEST_ASSERT_TRUE(transport.rejected > attempts);
    attempts = transport.rejected;
    sleepMs(1100);
    discovery->tick();                 // too early
    TEST_ASSERT_EQUAL(attempts, transport.rejected);
    sleepMs(1000);
    discovery->tick();                 // attempt 3
    TEST_ASSERT_TRUE(transport.rejected > attempts);

    // A new connection tries right away
    transport.rejectAll = false;
    transport.fireConnect();
    discovery->tick();
    TEST_ASSERT_EQUAL(1, transport.on(kDeviceTopic).size());
}

void test_removal_stub_kept_after_failed_publish(void) {
    addSensor("temp");
    addSensor("humidity");
    discovery->tick();
    discovery->removeEntity("sensor", "humidity");
    transport.messages.clear();
    transport.rejectAll = true;
    discovery->tick();
    transport.rejectAll = false;
    sleepMs(1100);
    discovery->tick();
    std::vector<MockTransport::Message> dev = transport.on(kDeviceTopic);
    TEST_ASSERT_EQUAL(2, dev.size());
    JsonDocument stub;
    deserializeJson(stub, dev[0].payload);
    TEST_ASSERT_EQUAL_STRING("sensor", stub["cmps"]["humidity"]["p"]);
    TEST_ASSERT_FALSE(lastDevicePayload()["cmps"]["humidity"].is<JsonObject>());
}

void test_remove_device_retried_after_failed_publish(void) {
    addSensor();
    discovery->tick();
    transport.messages.clear();
    transport.rejectAll = true;
    TEST_ASSERT_TRUE(discovery->removeDevice());
    TEST_ASSERT_EQUAL(0, transport.on(kDeviceTopic).size());
    transport.rejectAll = false;
    sleepMs(1100);
    discovery->tick();
    std::vector<MockTransport::Message> dev = transport.on(kDeviceTopic);
    TEST_ASSERT_EQUAL(1, dev.size());
    TEST_ASSERT_TRUE(dev[0].payload.empty());
}

void test_failed_migrate_message_holds_device_payload(void) {
    const char* oldTopic = "homeassistant/sensor/test_node/ip/config";
    discovery->migrateEntityDiscovery();
    HaSensorConfig ip;
    ip.common.object_id = "ip";
    discovery->publishSensorDiscovery(ip);
    transport.messages.clear();
    transport.fireConnect();
    discovery->tick();

    transport.rejectTopics = {oldTopic};
    transport.deliver(oldTopic, "{\"name\":\"IP\",\"uniq_id\":\"test_node_ip\"}");
    discovery->tick();
    TEST_ASSERT_EQUAL(1, transport.rejected);

    // The wait is over, but the migrate message isn't out: no device payload yet
    deliverMigrationSync();
    discovery->tick();
    TEST_ASSERT_EQUAL(0, transport.on(oldTopic).size());
    TEST_ASSERT_EQUAL(0, transport.on(kDeviceTopic).size());

    // The retry gets through, then the device payload follows
    transport.rejectTopics.clear();
    sleepMs(1100);
    discovery->tick();
    std::vector<MockTransport::Message> old = transport.on(oldTopic);
    TEST_ASSERT_EQUAL(1, old.size());
    TEST_ASSERT_EQUAL_STRING("{\"migrate_discovery\":true}", old[0].payload.c_str());
    TEST_ASSERT_EQUAL(1, transport.on(kDeviceTopic).size());
    TEST_ASSERT_TRUE(transport.indexOf(oldTopic) < transport.indexOf(kDeviceTopic));

    // Our migrate message coming back doesn't start a second migration
    transport.deliver(oldTopic, old[0].payload.c_str());
    discovery->tick();
    TEST_ASSERT_EQUAL(1, transport.on(oldTopic).size());

    sleepMs(5100);
    discovery->tick();
    old = transport.on(oldTopic);
    TEST_ASSERT_EQUAL(2, old.size());
    TEST_ASSERT_TRUE(old[1].payload.empty());
}

void test_failed_migration_clear_is_retried(void) {
    const char* oldTopic = "homeassistant/sensor/test_node/ip/config";
    discovery->migrateEntityDiscovery();
    HaSensorConfig ip;
    ip.common.object_id = "ip";
    discovery->publishSensorDiscovery(ip);
    transport.fireConnect();
    discovery->tick();
    transport.deliver(oldTopic, "{\"uniq_id\":\"test_node_ip\"}");
    discovery->tick();
    deliverMigrationSync();
    discovery->tick();
    transport.messages.clear();

    transport.rejectTopics = {oldTopic};
    sleepMs(5100);
    discovery->tick();
    TEST_ASSERT_EQUAL(1, transport.rejected);
    transport.rejectTopics.clear();
    sleepMs(1100);
    discovery->tick();
    std::vector<MockTransport::Message> old = transport.on(oldTopic);
    TEST_ASSERT_EQUAL(1, old.size());
    TEST_ASSERT_TRUE(old[0].payload.empty());
}

// ---------------------------------------------------------------------------
// MessageAssembler (messages handed over in parts)
// ---------------------------------------------------------------------------

static const uint8_t* bytes(const char* s) { return reinterpret_cast<const uint8_t*>(s); }

void test_assembler_puts_parts_together(void) {
    MessageAssembler a;
    TEST_ASSERT_TRUE(MessageAssembler::Result::Pending == a.add("t/1", bytes("hello "), 6, 0, 11));
    TEST_ASSERT_TRUE(MessageAssembler::Result::Complete == a.add("t/1", bytes("world"), 5, 6, 11));
    TEST_ASSERT_EQUAL_STRING("t/1", a.topic().c_str());
    TEST_ASSERT_EQUAL_STRING("hello world", a.payload().c_str());
    a.release();
    TEST_ASSERT_EQUAL(0, a.payload().size());

    // The next message starts over
    TEST_ASSERT_TRUE(MessageAssembler::Result::Pending == a.add("t/2", bytes("ab"), 2, 0, 3));
    TEST_ASSERT_TRUE(MessageAssembler::Result::Complete == a.add("t/2", bytes("c"), 1, 2, 3));
    TEST_ASSERT_EQUAL_STRING("abc", a.payload().c_str());
}

void test_assembler_drops_too_large(void) {
    MessageAssembler a(8);
    TEST_ASSERT_TRUE(MessageAssembler::Result::TooLarge == a.add("t", bytes("12345"), 5, 0, 10));
    TEST_ASSERT_TRUE(MessageAssembler::Result::Ignored == a.add("t", bytes("67890"), 5, 5, 10));
    // A fitting message afterwards works
    TEST_ASSERT_TRUE(MessageAssembler::Result::Pending == a.add("t", bytes("1234"), 4, 0, 8));
    TEST_ASSERT_TRUE(MessageAssembler::Result::Complete == a.add("t", bytes("5678"), 4, 4, 8));
}

void test_assembler_drops_broken_message(void) {
    MessageAssembler a;
    // A part is missing
    TEST_ASSERT_TRUE(MessageAssembler::Result::Pending == a.add("t", bytes("aa"), 2, 0, 6));
    TEST_ASSERT_TRUE(MessageAssembler::Result::Broken == a.add("t", bytes("cc"), 2, 4, 6));
    TEST_ASSERT_TRUE(MessageAssembler::Result::Ignored == a.add("t", bytes("cc"), 2, 4, 6));
    // Another topic in between
    TEST_ASSERT_TRUE(MessageAssembler::Result::Pending == a.add("t", bytes("aa"), 2, 0, 4));
    TEST_ASSERT_TRUE(MessageAssembler::Result::Broken == a.add("u", bytes("bb"), 2, 2, 4));
    // A first part without the rest, then a new message
    TEST_ASSERT_TRUE(MessageAssembler::Result::Pending == a.add("t", bytes("aa"), 2, 0, 4));
    TEST_ASSERT_TRUE(MessageAssembler::Result::Pending == a.add("v", bytes("xy"), 2, 0, 3));
    TEST_ASSERT_TRUE(MessageAssembler::Result::Complete == a.add("v", bytes("z"), 1, 2, 3));
    TEST_ASSERT_EQUAL_STRING("xyz", a.payload().c_str());
    // Longer than announced
    TEST_ASSERT_TRUE(MessageAssembler::Result::Pending == a.add("t", bytes("aa"), 2, 0, 3));
    TEST_ASSERT_TRUE(MessageAssembler::Result::Broken == a.add("t", bytes("bb"), 2, 2, 3));
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
    RUN_TEST(test_entity_category);
    RUN_TEST(test_expire_after);
    RUN_TEST(test_number_component_and_command);
    RUN_TEST(test_number_rejects_bad_range);
    RUN_TEST(test_select_component);
    RUN_TEST(test_text_component);
    RUN_TEST(test_light_components);
    RUN_TEST(test_parse_light_command);
    RUN_TEST(test_publish_state_light);
    RUN_TEST(test_cover_component_and_commands);
    RUN_TEST(test_cover_removed_drops_both_routes_and_states);
    RUN_TEST(test_republish_cover_without_position_drops_route);
    RUN_TEST(test_reconnect_backoff);
    RUN_TEST(test_reconnect_backoff_wraps);
    RUN_TEST(test_reconnect_now_sets_request_once);
    RUN_TEST(test_value_template_and_unique_id);
    RUN_TEST(test_publish_state_uses_state_topic_override);
    RUN_TEST(test_publish_state_topic_is_republished);
    RUN_TEST(test_default_schema_light_config);
    RUN_TEST(test_default_schema_light_commands_become_json);
    RUN_TEST(test_default_schema_light_state);
    RUN_TEST(test_on_message_and_subscribe);
    RUN_TEST(test_set_availability_topic);
    RUN_TEST(test_migrate_entity_discovery);
    RUN_TEST(test_no_migration_wait_by_default);
    RUN_TEST(test_migration_burst_is_not_dropped);
    RUN_TEST(test_migration_skips_unknown_unique_id);
    RUN_TEST(test_device_payload_retried_after_failed_publish);
    RUN_TEST(test_failed_availability_is_retried);
    RUN_TEST(test_publish_retry_backs_off);
    RUN_TEST(test_removal_stub_kept_after_failed_publish);
    RUN_TEST(test_remove_device_retried_after_failed_publish);
    RUN_TEST(test_failed_migrate_message_holds_device_payload);
    RUN_TEST(test_failed_migration_clear_is_retried);
    RUN_TEST(test_migration_waits_for_sync);
    RUN_TEST(test_migration_reads_only_what_it_needs);
    RUN_TEST(test_migration_wait_without_sync);
    RUN_TEST(test_assembler_puts_parts_together);
    RUN_TEST(test_assembler_drops_too_large);
    RUN_TEST(test_assembler_drops_broken_message);
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
