#ifdef ARDUINO_ARCH_ESP8266
#include <ESP8266WiFi.h>
#endif  // ARDUINO_ARCH_ESP8266

#ifdef ARDUINO_ARCH_ESP32
#include <WiFi.h>
#endif  // ARDUINO_ARCH_ESP32
#include <PubSubClient.h>
#include <HaDiscovery.h>
#include <transport/PubSubClientTransport.h>
#ifndef NO_JB_LOGGER
#include <jblogger.h>
#else
#include <JBLoggerDummy.h>
#endif

const char* ssid = "your-ssid";
const char* password = "your-password";
const char* mqtt_server = "your-mqtt-server";
const char* mqtt_user = "your-mqtt-user";
const char* mqtt_pass = "your-mqtt-password";

WiFiClient wifi;
PubSubClient mqtt(wifi);
PubSubClientTransport transport(mqtt);
HaDiscovery ha(transport, "homeassistant", "devices");
JBLogger logger("Main", LogLevel::LOG_LEVEL_DEBUG);

// Called when Home Assistant sends a command to a switch or button
void onCommand(const char* objectId, const char* payload) {
  logger.info("Command for %s: %s", objectId, payload);
  if (strcmp(objectId, "relay1") == 0) {
    bool on = strcmp(payload, "ON") == 0;
    // Switch the real relay here, then report the new state back
    ha.publishStateSwitch("relay1", on);
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {}  // Wait for Serial to be ready
  ha.setLogLevel(LOG_LEVEL_DEBUG);

  logger.info("Connecting to WiFi...");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  logger.info("WiFi connected");

  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String nodeId = "esp32_" + mac;

  // HaDiscovery copies these strings, so local Strings are fine here
  ha.setDevice({
    .node_id = nodeId.c_str(),
    .name = "Office Node",
    .manufacturer = "YourBrand",
    .model = "ESP32",
    .sw_version = "1.0.0",
    .identifiers = mac.c_str()
  });
  logger.info("node_id: %s", nodeId.c_str());

  transport.setServer(mqtt_server, 1883, mqtt_user, mqtt_pass);
  ha.onCommand(onCommand);
  // Connect through the transport so the Last Will and credentials are used
  transport.connect(nodeId.c_str());

  HaSwitchConfig sw{
    .common = { .object_id="relay1", .name="Relay" }
  };
  ha.publishSwitchDiscovery(sw);

  HaSensorConfig temp{
    .common = { .object_id="temperature", .name="Temperature" },
    .unit_of_measurement="°C",
    .device_class="temperature"
  };
  ha.publishSensorDiscovery(temp);
}

void loop() {
  mqtt.loop();
  ha.tick();

  static unsigned long lastMsg = 0;
  if (millis() - lastMsg > 5000) {
    lastMsg = millis();
    ha.publishState("temperature", "22.5");
  }
}
