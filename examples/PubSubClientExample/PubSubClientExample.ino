#ifdef ARDUINO_ARCH_ESP8266
#include <ESP8266WiFi.h>
#endif  // ARDUINO_ARCH_ESP8266

#ifdef ARDUINO_ARCH_ESP32
#include <WiFi.h>
#endif  // ARDUINO_ARCH_ESP32
#include <PubSubClient.h>
#include <HaDiscovery.h>
#include <transport/PubSubClientTransport.h>
#include <jblogger.h>

const char* ssid = "your-ssid";
const char* password = "your-password";
const char* mqtt_server = "your-mqtt-server";
const char* mqtt_user = "your-mqtt-user";
const char* mqtt_pass = "your-mqtt-password";

WiFiClient wifi;
PubSubClient mqtt(wifi);
PubSubClientTransport transport(mqtt);
HaDiscovery ha(transport, "homeassistant", "devices");

void setup() {
  Serial.begin(115200);
  ha.setLogLevel(LOG_LEVEL_DEBUG);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String nodeId = "esp32_" + mac;

  ha.setDevice({
    .node_id = nodeId.c_str(),
    .name = "Office Node",
    .manufacturer = "YourBrand",
    .model = "ESP32",
    .sw_version = "1.0.0",
    .identifiers = mac.c_str()
  });

  transport.setServer(mqtt_server, 1883, mqtt_user, mqtt_pass);
  mqtt.connect(nodeId.c_str(), mqtt_user, mqtt_pass);

  HaSwitchConfig sw{
    .common = { .object_id="relay1", .name="Desk Relay" }
  };
  ha.publishSwitchDiscovery(sw);

  HaSensorConfig temp{
    .common = { .object_id="temperature", .name="Office Temperature" },
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
