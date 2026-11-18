#ifdef ARDUINO_ARCH_ESP8266
#include <ESP8266WiFi.h>
#endif  // ARDUINO_ARCH_ESP8266

#ifdef ARDUINO_ARCH_ESP32
#include <WiFi.h>
#endif  // ARDUINO_ARCH_ESP32
#include <AsyncMqttClient.h>
#include <HaDiscovery.h>
#include <transport/AsyncMqttClientTransport.h>
#ifndef NO_JB_LOGGER
#include <jblogger.h>
#else
#include <JBLoggerDummy.h>
#endif

const char* ssid = "your-ssid";
const char* password = "your-password";
const char* mqtt_host = "your-mqtt-server";
const char* mqtt_user = "your-mqtt-user";
const char* mqtt_pass = "your-mqtt-password";

AsyncMqttClient mqtt;
AsyncMqttClientTransport transport(mqtt);
HaDiscovery ha(transport);
JBLogger logger("Main");

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
  logger.setLogLevel(LOG_LEVEL_DEBUG);
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

  ha.setDevice({
    .node_id = nodeId.c_str(),
    .name = "Kitchen Node",
    .manufacturer = "YourBrand",
    .model = "ESP32",
    .sw_version = "1.0.0",
    .identifiers = mac.c_str()
  });

  transport.setServer(mqtt_host, 1883, mqtt_user, mqtt_pass);
  ha.onCommand(onCommand);
  // Connect through the transport so the Last Will and credentials are used
  transport.connect(nodeId.c_str());

  HaSensorConfig temp{
    .common = { .object_id="temperature", .name="Kitchen Temperature" },
    .unit_of_measurement="°C",
    .device_class="temperature",
    .state_class="measurement"
  };
  ha.publishSensorDiscovery(temp);

  HaSwitchConfig relay{
    .common = { .object_id="relay1", .name="Kitchen Light" }
  };
  ha.publishSwitchDiscovery(relay);
}

void loop() {
  ha.tick();  // Re-publishes discovery after Home Assistant restarts

  static unsigned long lastMsg = 0;
  if (millis() - lastMsg > 5000) {
    lastMsg = millis();
    ha.publishState("temperature", "23.5");
  }
}
