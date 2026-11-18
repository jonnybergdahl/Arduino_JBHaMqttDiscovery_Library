// Hardware test firmware for JBHaMqttDiscovery.
//
// Publishes one device with every entity type and option the library supports,
// and takes test commands on the USB serial port (one per line):
//
//   status                     print connection state
//   remove <component> <id>    ha.removeEntity()
//   readd                      add all entities again
//   removedevice               ha.removeDevice()
//   republish                  ha.republishAll()
//   mqttdrop                   drop the MQTT connection (auto-reconnect should follow)
//   wifidrop                   drop Wi-Fi for 15 s
//   alt online|offline         publish the availability override topic
//   restart                    ESP.restart()

#include <Arduino.h>
#include <WiFi.h>
#include <HaDiscovery.h>
#include <jblogger.h>
#include "secrets.h"

#ifdef USE_ASYNC
#include <AsyncMqttClient.h>
#include <transport/AsyncMqttClientTransport.h>
AsyncMqttClient mqtt;
AsyncMqttClientTransport transport(mqtt);
static const char* kTransportName = "AsyncMqttClient";
#else
#include <PubSubClient.h>
#include <transport/PubSubClientTransport.h>
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
PubSubClientTransport transport(mqtt);
static const char* kTransportName = "PubSubClient";
#endif

HaDiscovery ha(transport, "homeassistant", "devices", LOG_LEVEL_DEBUG);
JBLogger logger("HwTest", LOG_LEVEL_DEBUG);

static const int BOOT_BUTTON_PIN = 0;

std::string nodeId;
std::string altAvailTopic;
bool relayOn = false;
bool lastButton = false;
unsigned long pings = 0;
unsigned long wifiBackAt = 0;

void onCommand(const char* objectId, const char* payload) {
  logger.info("Command for %s: %s", objectId, payload);
  if (strcmp(objectId, "relay") == 0) {
    relayOn = strcmp(payload, "ON") == 0;
    ha.publishStateSwitch("relay", relayOn);
  } else if (strcmp(objectId, "ping") == 0) {
    pings++;
    ha.publishState("pings", std::to_string(pings));
  }
}

void addEntities() {
  HaSensorConfig uptime{
    .common = { .object_id = "uptime", .name = "Uptime", .icon = "mdi:timer-outline",
                .entity_id = "jbhatest_uptime" },
    .unit_of_measurement = "s",
    .device_class = "duration",
    .state_class = "total_increasing"
  };
  ha.publishSensorDiscovery(uptime);

  HaSensorConfig rssi{
    .common = { .object_id = "rssi", .name = "Wi-Fi signal" },
    .unit_of_measurement = "dBm",
    .device_class = "signal_strength",
    .state_class = "measurement"
  };
  ha.publishSensorDiscovery(rssi);

  HaSensorConfig heap{
    .common = { .object_id = "free_heap", .name = "Free heap", .icon = "mdi:memory" },
    .unit_of_measurement = "B",
    .device_class = "data_size",
    .state_class = "measurement"
  };
  ha.publishSensorDiscovery(heap);

  // Main feature: shows with just the device name
  HaSensorConfig chipTemp{
    .common = { .object_id = "chip_temp", .use_device_name = true },
    .unit_of_measurement = "°C",
    .device_class = "temperature",
    .state_class = "measurement"
  };
  ha.publishSensorDiscovery(chipTemp);

  HaSensorConfig pingCount{
    .common = { .object_id = "pings", .name = "Pings", .icon = "mdi:counter" },
    .state_class = "total_increasing"
  };
  ha.publishSensorDiscovery(pingCount);

  // Uses its own availability topic instead of the shared one
  HaSensorConfig alt{
    .common = { .object_id = "alt_avail", .name = "Alt availability",
                .availability_topic_override = altAvailTopic.c_str() },
  };
  ha.publishSensorDiscovery(alt);

  HaSwitchConfig relay{
    .common = { .object_id = "relay", .name = "Relay", .icon = "mdi:electric-switch" }
  };
  ha.publishSwitchDiscovery(relay);

  HaBinarySensorConfig button{
    .common = { .object_id = "boot_button", .name = "BOOT button" }
  };
  ha.publishBinarySensorDiscovery(button);

  HaButtonConfig ping{
    .common = { .object_id = "ping", .name = "Ping", .icon = "mdi:gesture-tap-button" }
  };
  ha.publishButtonDiscovery(ping);
}

void publishSensors() {
  ha.publishState("uptime", std::to_string(millis() / 1000));
  ha.publishState("rssi", std::to_string(WiFi.RSSI()));
  ha.publishState("free_heap", std::to_string(ESP.getFreeHeap()));
  char t[16];
  snprintf(t, sizeof(t), "%.1f", temperatureRead());
  ha.publishState("chip_temp", t);
  ha.publishState("alt_avail", "42");
}

void publishAlt(const char* payload) {
  transport.publish(altAvailTopic.c_str(), reinterpret_cast<const uint8_t*>(payload), strlen(payload), true, 1);
}

void handleSerial() {
  static std::string line;
  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c != '\n' && c != '\r') {
      line += c;
      continue;
    }
    if (line.empty()) {
      continue;
    }
    std::string cmd = line;
    line.clear();
    logger.info(">>> %s", cmd.c_str());

    if (cmd == "status") {
      logger.info("transport=%s wifi=%d mqtt=%d heap=%u", kTransportName,
                  WiFi.status() == WL_CONNECTED, transport.connected(), ESP.getFreeHeap());
    } else if (cmd.rfind("remove ", 0) == 0) {
      size_t sp = cmd.find(' ', 7);
      if (sp == std::string::npos) {
        logger.error("usage: remove <component> <object_id>");
      } else {
        std::string component = cmd.substr(7, sp - 7);
        std::string id = cmd.substr(sp + 1);
        logger.info("removeEntity(%s, %s) = %d", component.c_str(), id.c_str(), ha.removeEntity(component, id));
      }
    } else if (cmd == "readd") {
      addEntities();
      publishSensors();
    } else if (cmd == "removedevice") {
      logger.info("removeDevice() = %d", ha.removeDevice());
    } else if (cmd == "republish") {
      ha.republishAll();
    } else if (cmd == "mqttdrop") {
      // Close the socket underneath the client, like a broker restart
#ifdef USE_ASYNC
      mqtt.disconnect(true);
#else
      wifiClient.stop();
#endif
    } else if (cmd == "wifidrop") {
      WiFi.disconnect();
      wifiBackAt = millis() + 15000;
    } else if (cmd == "alt online") {
      publishAlt("online");
    } else if (cmd == "alt offline") {
      publishAlt("offline");
    } else if (cmd == "restart") {
      ESP.restart();
    } else {
      logger.error("unknown command");
    }
  }
}

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) {}
  delay(500);
  logger.info("JBHaMqttDiscovery hardware test, transport %s", kTransportName);

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);

  // Show whether the SSID is visible at all (ESP32 is 2.4 GHz only)
  int n = WiFi.scanNetworks();
  int matches = 0;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == WIFI_SSID) {
      matches++;
      logger.info("Found %s: RSSI %d, channel %d, auth %d", WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                  WiFi.channel(i), (int)WiFi.encryptionType(i));
    }
  }
  logger.info("Scan found %d networks, %d named %s", n, matches, WIFI_SSID);

  // 15/202/204 = wrong password or auth failure, 201 = network not found
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    logger.warning("Wi-Fi disconnected, reason %d", (int)info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
    logger.info("Wi-Fi got IP %s", WiFi.localIP().toString().c_str());
    transport.reconnectNow();
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);

#ifdef WIFI_LOW_TX_POWER
  // Works around AUTH_EXPIRE (reason 2) on some ESP32-S3 boards
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  logger.info("Wi-Fi TX power lowered to 8.5 dBm");
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
#ifdef WIFI_LOW_TX_POWER
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
#endif
  logger.info("Connecting to Wi-Fi %s", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
  }
  logger.info("Wi-Fi connected, IP %s", WiFi.localIP().toString().c_str());

  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toLowerCase();
  nodeId = std::string("jbhatest_") + mac.c_str();
  altAvailTopic = "devices/" + nodeId + "/alt_status";

  ha.setDevice({
    .node_id = nodeId.c_str(),
    .name = "JBHa Test",
    .manufacturer = "Unexpected Maker",
    .model = "TinyS3",
    .sw_version = kTransportName,
    .identifiers = mac.c_str()
  });
  ha.onCommand(onCommand);

  transport.setServer(MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS);
  transport.connect(nodeId.c_str());

  addEntities();
  publishSensors();
}

void loop() {
#ifndef USE_ASYNC
  mqtt.loop();
#endif
  ha.tick();
  handleSerial();

  // The override availability topic isn't managed by the library
  static bool wasConnected = false;
  if (transport.connected() && !wasConnected) {
    publishAlt("online");
  }
  wasConnected = transport.connected();

  if (wifiBackAt && millis() > wifiBackAt) {
    wifiBackAt = 0;
    logger.info("Turning Wi-Fi back on");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  bool pressed = digitalRead(BOOT_BUTTON_PIN) == LOW;
  if (pressed != lastButton) {
    lastButton = pressed;
    ha.publishState("boot_button", pressed ? "ON" : "OFF");
  }

  static unsigned long lastPublish = 0;
  if (millis() - lastPublish > 10000) {
    lastPublish = millis();
    publishSensors();
  }
}
