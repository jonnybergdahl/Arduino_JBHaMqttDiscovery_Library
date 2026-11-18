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
//   pause                      stop/start sensor updates (uptime expires after 30 s)
//   restart                    ESP.restart()
//
// Builds for the TinyS3 (ESP32-S3) and the Wemos D1 mini (ESP8266).

#include <Arduino.h>
#ifdef ESP8266
#include <ESP8266WiFi.h>
static const char* kBoardName = "JBHa Test D1";
static const char* kManufacturer = "Wemos";
static const char* kModel = "D1 mini";
WiFiEventHandler gotIpHandler;
WiFiEventHandler disconnectedHandler;
#else
#include <WiFi.h>
static const char* kBoardName = "JBHa Test";
static const char* kManufacturer = "Unexpected Maker";
static const char* kModel = "TinyS3";
#endif
#include <HaDiscovery.h>
#include <jblogger.h>
#include "secrets.h"

#if defined(USE_ASYNC)
#include <AsyncMqttClient.h>
#include <transport/AsyncMqttClientTransport.h>
AsyncMqttClient mqtt;
AsyncMqttClientTransport transport(mqtt);
static const char* kTransportName = "AsyncMqttClient";
#elif defined(USE_ESPMQTT)
#include <espMqttClient.h>
#include <transport/EspMqttClientTransport.h>
espMqttClient mqtt;
EspMqttClientTransport<espMqttClient> transport(mqtt);
static const char* kTransportName = "espMqttClient";
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
bool sensorsPaused = false;
unsigned long reportIntervalMs = 10000;

// Light
bool ledOn = false;
uint16_t ledBrightness = 128;
uint8_t ledR = 255, ledG = 120, ledB = 0;

// Simulated cover: moves 10% per second towards coverTarget
int coverPosition = 0;
int coverTarget = 0;
unsigned long lastCoverStep = 0;

void applyLed() {
#ifdef ESP8266
  // Built-in LED on GPIO2, active low
  analogWrite(LED_BUILTIN, ledOn ? 255 - ledBrightness : 255);
#elif defined(RGB_BUILTIN)
  float k = ledOn ? ledBrightness / 255.0f : 0;
  rgbLedWrite(RGB_BUILTIN, ledR * k, ledG * k, ledB * k);
#endif
}

void publishLight() {
  HaLightState st;
  st.on = ledOn;
  st.brightness = ledBrightness;
#ifndef ESP8266
  st.r = ledR;
  st.g = ledG;
  st.b = ledB;
#endif
  ha.publishStateLight("led", st);
}

void publishCover() {
  const char* state;
  if (coverPosition < coverTarget) {
    state = "opening";
  } else if (coverPosition > coverTarget) {
    state = "closing";
  } else {
    state = coverPosition == 0 ? "closed" : "open";
  }
  ha.publishState("blind", state);
  ha.publishStatePosition("blind", coverPosition);
}

void onCommand(const char* objectId, const char* payload) {
  logger.info("Command for %s: %s", objectId, payload);
  if (strcmp(objectId, "relay") == 0) {
    relayOn = strcmp(payload, "ON") == 0;
    ha.publishStateSwitch("relay", relayOn);
  } else if (strcmp(objectId, "ping") == 0) {
    pings++;
    ha.publishState("pings", std::to_string(pings));
  } else if (strcmp(objectId, "interval") == 0) {
    reportIntervalMs = static_cast<unsigned long>(atof(payload) * 1000);
    ha.publishStateNumber("interval", reportIntervalMs / 1000.0, 0);
  } else if (strcmp(objectId, "mode") == 0 || strcmp(objectId, "message") == 0) {
    ha.publishState(objectId, payload);
  } else if (strcmp(objectId, "led") == 0) {
    HaLightCommand cmd;
    if (HaDiscovery::parseLightCommand(payload, cmd)) {
      if (cmd.has_state) ledOn = cmd.on;
      if (cmd.has_brightness) ledBrightness = cmd.brightness;
      if (cmd.has_color) { ledR = cmd.r; ledG = cmd.g; ledB = cmd.b; }
      applyLed();
      publishLight();
    }
  } else if (strcmp(objectId, "blind") == 0) {
    if (strcmp(payload, "OPEN") == 0) {
      coverTarget = 100;
    } else if (strcmp(payload, "CLOSE") == 0) {
      coverTarget = 0;
    } else if (strcmp(payload, "STOP") == 0) {
      coverTarget = coverPosition;
    } else {
      coverTarget = constrain(atoi(payload), 0, 100);
    }
    publishCover();
  }
}

void addEntities() {
  HaSensorConfig uptime{
    .common = { .object_id = "uptime", .name = "Uptime", .icon = "mdi:timer-outline",
                .entity_id = "jbhatest_uptime" },
    .unit_of_measurement = "s",
    .device_class = "duration",
    .state_class = "total_increasing",
    .expire_after = 30
  };
  ha.publishSensorDiscovery(uptime);

  HaSensorConfig rssi{
    .common = { .object_id = "rssi", .name = "Wi-Fi signal",
                .entity_category = HaEntityCategory::Diagnostic },
    .unit_of_measurement = "dBm",
    .device_class = "signal_strength",
    .state_class = "measurement"
  };
  ha.publishSensorDiscovery(rssi);

  HaSensorConfig heap{
    .common = { .object_id = "free_heap", .name = "Free heap", .icon = "mdi:memory",
                .entity_category = HaEntityCategory::Diagnostic },
    .unit_of_measurement = "B",
    .device_class = "data_size",
    .state_class = "measurement"
  };
  ha.publishSensorDiscovery(heap);

  // Main feature: shows with just the device name
#ifdef ESP8266
  // The ESP8266 has no temperature sensor
  HaSensorConfig chipTemp{
    .common = { .object_id = "heap_frag", .icon = "mdi:memory", .use_device_name = true },
    .unit_of_measurement = "%",
    .state_class = "measurement"
  };
#else
  HaSensorConfig chipTemp{
    .common = { .object_id = "chip_temp", .use_device_name = true },
    .unit_of_measurement = "°C",
    .device_class = "temperature",
    .state_class = "measurement"
  };
#endif
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

  HaNumberConfig interval{
    .common = { .object_id = "interval", .name = "Report interval", .icon = "mdi:timer-cog-outline",
                .entity_category = HaEntityCategory::Config },
    .min = 5, .max = 60, .step = 5,
    .mode = HaNumberMode::Slider,
    .unit_of_measurement = "s"
  };
  ha.publishNumberDiscovery(interval);

  HaSelectConfig mode{
    .common = { .object_id = "mode", .name = "Mode", .icon = "mdi:tune" },
    .options = {"Eco", "Normal", "Turbo"}
  };
  ha.publishSelectDiscovery(mode);

  HaTextConfig message{
    .common = { .object_id = "message", .name = "Message", .icon = "mdi:message-text" },
    .max_length = 32
  };
  ha.publishTextDiscovery(message);

  HaLightConfig led{
    .common = { .object_id = "led", .name = "LED" },
#ifdef ESP8266
    .color_mode = HaLightColorMode::Brightness
#else
    .color_mode = HaLightColorMode::Rgb
#endif
  };
  ha.publishLightDiscovery(led);

  HaCoverConfig blind{
    .common = { .object_id = "blind", .name = "Blind" },
    .device_class = "blind",
    .has_position = true
  };
  ha.publishCoverDiscovery(blind);
}

void publishSettings() {
  ha.publishStateNumber("interval", reportIntervalMs / 1000.0, 0);
  ha.publishState("mode", "Normal");
  ha.publishState("message", "Hello");
  publishLight();
  publishCover();
}

void publishSensors() {
  ha.publishState("uptime", std::to_string(millis() / 1000));
  ha.publishState("rssi", std::to_string(WiFi.RSSI()));
  ha.publishState("free_heap", std::to_string(ESP.getFreeHeap()));
#ifdef ESP8266
  ha.publishState("heap_frag", std::to_string(ESP.getHeapFragmentation()));
#else
  char t[16];
  snprintf(t, sizeof(t), "%.1f", temperatureRead());
  ha.publishState("chip_temp", t);
#endif
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
      publishSettings();
    } else if (cmd == "removedevice") {
      logger.info("removeDevice() = %d", ha.removeDevice());
    } else if (cmd == "republish") {
      ha.republishAll();
    } else if (cmd == "mqttdrop") {
      // Close the socket underneath the client, like a broker restart
#if defined(USE_ASYNC) || defined(USE_ESPMQTT)
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
    } else if (cmd == "pause") {
      sensorsPaused = !sensorsPaused;
      logger.info("Sensor updates %s", sensorsPaused ? "paused" : "resumed");
    } else if (cmd == "restart") {
      ESP.restart();
    } else {
      logger.error("unknown command");
    }
  }
}

void setup() {
  Serial.begin(115200);
#ifdef ESP32
  // Drop output when no one reads the USB port, instead of blocking loop()
  Serial.setTxTimeoutMs(0);
#endif
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) {}
  delay(500);
  logger.info("JBHaMqttDiscovery hardware test, transport %s", kTransportName);

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
#ifdef ESP8266
  pinMode(LED_BUILTIN, OUTPUT);
#endif
  applyLed();

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
#ifdef ESP8266
  disconnectedHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& e) {
    logger.warning("Wi-Fi disconnected, reason %d", (int)e.reason);
  });
  gotIpHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& e) {
    logger.info("Wi-Fi got IP %s", e.ip.toString().c_str());
    transport.reconnectNow();
  });
#else
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    logger.warning("Wi-Fi disconnected, reason %d", (int)info.wifi_sta_disconnected.reason);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) {
    logger.info("Wi-Fi got IP %s", WiFi.localIP().toString().c_str());
    transport.reconnectNow();
  }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
#endif

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
  std::string visitUrl = std::string("http://") + WiFi.localIP().toString().c_str() + "/";

  ha.setDevice({
    .node_id = nodeId.c_str(),
    .name = kBoardName,
    .manufacturer = kManufacturer,
    .model = kModel,
    .sw_version = kTransportName,
    .identifiers = mac.c_str(),
    .configuration_url = visitUrl.c_str()
  });
  ha.onCommand(onCommand);

  transport.setServer(MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS);
  transport.connect(nodeId.c_str());

  addEntities();
  publishSensors();
  publishSettings();
}

void loop() {
#if !defined(USE_ASYNC) && !defined(USE_ESPMQTT)
  mqtt.loop();  // PubSubClient; the espMqttClient transport runs its client from tick()
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
  if (millis() - lastPublish > reportIntervalMs) {
    lastPublish = millis();
    if (!sensorsPaused) {
      publishSensors();
    }
  }

  if (coverPosition != coverTarget && millis() - lastCoverStep > 1000) {
    lastCoverStep = millis();
    coverPosition += coverTarget > coverPosition ? 10 : -10;
    if ((coverTarget - coverPosition) * (coverTarget > coverPosition ? 1 : -1) < 0) {
      coverPosition = coverTarget;
    }
    publishCover();
  }
}
