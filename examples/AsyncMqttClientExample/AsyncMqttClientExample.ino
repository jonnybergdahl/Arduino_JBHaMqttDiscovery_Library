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

// Reconnect MQTT right away when Wi-Fi comes back, instead of waiting for the next retry
#ifdef ARDUINO_ARCH_ESP8266
WiFiEventHandler gotIpHandler;
#endif

void watchWiFi() {
#ifdef ARDUINO_ARCH_ESP8266
  gotIpHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP&) { transport.reconnectNow(); });
#endif
#ifdef ARDUINO_ARCH_ESP32
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) { transport.reconnectNow(); }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
#endif
}

// Called from ha.tick() when Home Assistant sends a command
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
  // Give a connected computer up to 2 s to open the port, but don't wait for one:
  // the board normally runs from a power supply with nothing on the USB port.
  unsigned long serialStart = millis();
  while (!Serial && millis() - serialStart < 2000) {
    delay(10);
  }
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  // ESP32 native USB (USB-Serial/JTAG): drop log output when no computer reads it,
  // instead of blocking loop()
  Serial.setTxTimeoutMs(0);
#endif
  logger.setLogLevel(LOG_LEVEL_DEBUG);
  ha.setLogLevel(LOG_LEVEL_DEBUG);

  logger.info("Connecting to WiFi...");
  watchWiFi();
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  logger.info("WiFi connected");

  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String nodeId = "node_" + mac;  // letters, digits, '_' and '-' only

  ha.setDevice({
    .node_id = nodeId.c_str(),
    .name = "Kitchen Node",
    .manufacturer = "YourBrand",
#ifdef ARDUINO_ARCH_ESP8266
    .model = "ESP8266",
#else
    .model = "ESP32",
#endif
    .sw_version = "1.0.0",
    .identifiers = mac.c_str()
  });

  transport.setServer(mqtt_host, 1883, mqtt_user, mqtt_pass);
  ha.onCommand(onCommand);
  // Connect through the transport so the Last Will and credentials are used
  transport.connect(nodeId);

  HaSensorConfig temp{
    .common = { .object_id="temperature", .name="Temperature" },
    .unit_of_measurement="°C",
    .device_class="temperature",
    .state_class="measurement"
  };
  ha.publishSensorDiscovery(temp);

  HaSwitchConfig relay{
    .common = { .object_id="relay1", .name="Light" }
  };
  ha.publishSwitchDiscovery(relay);
}

void loop() {
  ha.tick();  // REQUIRED: publishes discovery, handles commands, reconnects

  static unsigned long lastMsg = 0;
  if (millis() - lastMsg > 5000) {
    lastMsg = millis();
    ha.publishState("temperature", "23.5");
  }
}
