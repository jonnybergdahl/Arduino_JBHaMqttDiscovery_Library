#include <unity.h>
#include <string>
#include <vector>
#include <cstring>
#include "HaDiscovery.h"
#include "transport/MqttTransport.h"
#include <ArduinoJson.h>

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
    bool isConnected = true;
    void (*onConnectCb)(void*) = nullptr;
    void* onConnectCtx = nullptr;

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
    }
};

MockTransport transport;
HaDiscovery* discovery;

void setUp(void) {
    transport.clear();
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

void test_sensor_discovery(void) {
    HaSensorConfig cfg;
    cfg.common.object_id = "temp";
    cfg.common.name = "Temperature";
    cfg.unit_of_measurement = "°C";
    cfg.device_class = "temperature";
    cfg.state_class = "measurement";

    bool ok = discovery->publishSensorDiscovery(cfg);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, transport.messages.size());

    const auto& msg = transport.messages[0];
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/test_node/temp/config", msg.topic.c_str());
    TEST_ASSERT_TRUE(msg.retained);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, msg.payload);
    TEST_ASSERT_FALSE(error);

    TEST_ASSERT_EQUAL_STRING("Temperature", doc["name"]);
    TEST_ASSERT_EQUAL_STRING("test_node_temp", doc["uniq_id"]);
    TEST_ASSERT_EQUAL_STRING("devices/test_node/temp/state", doc["stat_t"]);
    TEST_ASSERT_EQUAL_STRING("devices/test_node/status", doc["avty_t"]);
    TEST_ASSERT_EQUAL_STRING("°C", doc["unit_of_meas"]);
    TEST_ASSERT_EQUAL_STRING("temperature", doc["dev_cla"]);
    TEST_ASSERT_EQUAL_STRING("measurement", doc["stat_cla"]);

    JsonObject dev = doc["dev"];
    TEST_ASSERT_EQUAL_STRING("Test Device", dev["name"]);
    TEST_ASSERT_EQUAL_STRING("test_mac", dev["ids"][0]);
}

void test_switch_discovery(void) {
    HaSwitchConfig cfg;
    cfg.common.object_id = "relay";
    cfg.common.name = "Relay";

    bool ok = discovery->publishSwitchDiscovery(cfg);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, transport.messages.size());

    const auto& msg = transport.messages[0];
    TEST_ASSERT_EQUAL_STRING("homeassistant/switch/test_node/relay/config", msg.topic.c_str());

    JsonDocument doc;
    deserializeJson(doc, msg.payload);
    TEST_ASSERT_EQUAL_STRING("devices/test_node/relay/set", doc["cmd_t"]);
    TEST_ASSERT_EQUAL_STRING("ON", doc["pl_on"]);
    TEST_ASSERT_EQUAL_STRING("OFF", doc["pl_off"]);
}

void test_binary_sensor_discovery(void) {
    HaBinarySensorConfig cfg;
    cfg.common.object_id = "motion";
    cfg.device_class = "motion";

    bool ok = discovery->publishBinarySensorDiscovery(cfg);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, transport.messages.size());

    const auto& msg = transport.messages[0];
    TEST_ASSERT_EQUAL_STRING("homeassistant/binary_sensor/test_node/motion/config", msg.topic.c_str());

    JsonDocument doc;
    deserializeJson(doc, msg.payload);
    TEST_ASSERT_EQUAL_STRING("motion", doc["dev_cla"]);
    TEST_ASSERT_EQUAL_STRING("ON", doc["pl_on"]);
}

void test_button_discovery(void) {
    HaButtonConfig cfg;
    cfg.common.object_id = "restart";
    cfg.common.name = "Restart Device";

    bool ok = discovery->publishButtonDiscovery(cfg);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, transport.messages.size());

    const auto& msg = transport.messages[0];
    TEST_ASSERT_EQUAL_STRING("homeassistant/button/test_node/restart/config", msg.topic.c_str());

    JsonDocument doc;
    deserializeJson(doc, msg.payload);
    TEST_ASSERT_EQUAL_STRING("PRESS", doc["pl_prs"]);
}

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

void test_remove_entity(void) {
    discovery->removeEntity("sensor", "temp");
    TEST_ASSERT_EQUAL(1, transport.messages.size());
    TEST_ASSERT_EQUAL_STRING("homeassistant/sensor/test_node/temp/config", transport.messages[0].topic.c_str());
    TEST_ASSERT_TRUE(transport.messages[0].payload.empty());
    TEST_ASSERT_TRUE(transport.messages[0].retained);
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

void test_press_button(void) {
    discovery->pressButton("restart");
    TEST_ASSERT_EQUAL(1, transport.messages.size());
    TEST_ASSERT_EQUAL_STRING("devices/test_node/restart/set", transport.messages[0].topic.c_str());
    TEST_ASSERT_EQUAL_STRING("PRESS", transport.messages[0].payload.c_str());

    transport.clear();
    discovery->pressButton("restart", "CUSTOM");
    TEST_ASSERT_EQUAL_STRING("CUSTOM", transport.messages[0].payload.c_str());
}

// Support for native environment where setup/loop might not be enough for unity runner
#if defined(ARDUINO)
void setup() {
    delay(2000);
    UNITY_BEGIN();
    RUN_TEST(test_sensor_discovery);
    RUN_TEST(test_switch_discovery);
    RUN_TEST(test_binary_sensor_discovery);
    RUN_TEST(test_button_discovery);
    RUN_TEST(test_publish_state);
    RUN_TEST(test_publish_state_switch);
    RUN_TEST(test_remove_entity);
    RUN_TEST(test_availability);
    RUN_TEST(test_press_button);
    UNITY_END();
}

void loop() {}
#else
int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_sensor_discovery);
    RUN_TEST(test_switch_discovery);
    RUN_TEST(test_binary_sensor_discovery);
    RUN_TEST(test_button_discovery);
    RUN_TEST(test_publish_state);
    RUN_TEST(test_publish_state_switch);
    RUN_TEST(test_remove_entity);
    RUN_TEST(test_availability);
    RUN_TEST(test_press_button);
    return UNITY_END();
}
#endif
