# Arduino_JBHaMqttDiscovery
An Arduino library for Home Assistant MQTT Auto Discovery

## Usage

### Dependencies

1. [ArduinoJson](https://arduinojson.org/)
2. [PubSubClient](https://pubsubclient.knolleary.net/) or [AsyncMqttClient](https://github.com/marvinroger/async-mqtt-client)
3. [JBLogger](https://github.com/jonnybergdahl/Arduino_JBLogger_Library)

### PubSubClient (polling required)

```c++
#include <PubSubClient.h>
#include <HaDiscovery.h>
#include <transport/PubSubClientTransport.h>

WiFiClient wifi;
PubSubClient mqtt(wifi);
PubSubClientTransport transport(mqtt);
HaDiscovery ha(transport);

void setup() {
  ha.setDevice({
    .node_id = "esp8266_office_01",
    .name = "Office Node",
    .manufacturer = "YourBrand",
    .model = "ESP8266",
    .sw_version = "1.0.0",
    .identifiers = "112233445566"
  });

  // Configure mqtt server/credentials externally, then connect:
  // mqtt.setServer(...); mqtt.connect(...);

  HaSwitchConfig sw{
    .common = { .object_id="relay1", .name="Desk Relay" }
  };
  ha.publishSwitchDiscovery(sw);
}

void loop() {
  mqtt.loop(); // REQUIRED
  ha.tick();   // Recommended: enables auto "on-connect" availability publish
}
```

### AsyncMqttClient (event-driven, no polling required)
```c++
#include <AsyncMqttClient.h>
#include <HaDiscovery.h>
#include <transport/AsyncMqttClientTransport.h>

AsyncMqttClient mqtt;
AsyncMqttClientTransport transport(mqtt);
HaDiscovery ha(transport);

void setup() {
  ha.setDevice({
    .node_id = "esp32_kitchen_01",
    .name = "Kitchen Node",
    .manufacturer = "YourBrand",
    .model = "ESP32",
    .sw_version = "1.0.0",
    .identifiers = "AABBCCDDEEFF"
  });

  // Configure mqtt host/credentials/LWT externally, then connect:
  mqtt.connect();

  HaSensorConfig temp{
    .common = { .object_id="temperature", .name="Kitchen Temperature" },
    .unit_of_measurement="°C",
    .device_class="temperature",
    .state_class="measurement"
  };
  ha.publishSensorDiscovery(temp);
}

void loop() {
  // nothing required for HaDiscovery
}

## Entitity usage

### Binary sensor

```c++
HaBinarySensorConfig motion{
  .common = { .object_id="motion", .name="Hallway Motion", .icon="mdi:motion-sensor" },
  .device_class = "motion"
};
ha.publishBinarySensorDiscovery(motion);
```

### Button

```c++
HaButtonConfig restart{
  .common = { .object_id="restart", .name="Restart Device", .icon="mdi:restart" },
  .payload_press = "PRESS"
};
ha.publishButtonDiscovery(restart);

// later:
ha.pressButton("restart");
```
