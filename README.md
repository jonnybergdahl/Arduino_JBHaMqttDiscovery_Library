# JBHaMqttDiscovery
An Arduino library for Home Assistant MQTT Auto Discovery.

## What is this library?
**JBHaMqttDiscovery** makes sensors and devices connected to Arduino/ESP8266/ESP32 devices show up automatically in **Home Assistant** via **MQTT Discovery**.

It helps you builds the correct *discovery topics* and *JSON configuration payloads* and publishes them as **retained** messages, so Home Assistant can create entities (e.g. sensors, switches, binary sensors, buttons) without manual UI/YAML setup.

It is also **transport-agnostic**: you can use different MQTT client libraries through a small “transport” adapter (e.g. PubSubClient or AsyncMqttClient), while keeping the same discovery API in your firmware.

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

  HaSensorConfig temp{
    .common = { .object_id="temperature", .name="Office Temperature" },
    .unit_of_measurement="°C",
    .device_class="temperature"
  };
  ha.publishSensorDiscovery(temp);
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

  HaSwitchConfig relay{
    .common = { .object_id="relay1", .name="Kitchen Light" }
  };
  ha.publishSwitchDiscovery(relay);
}

void loop() {
  // nothing required for HaDiscovery
}
```

## Entity usage

### Sensor

```c++
HaSensorConfig temp{
  .common = { .object_id="temperature", .name="Kitchen Temperature", .icon="mdi:thermometer" },
  .unit_of_measurement="°C",
  .device_class="temperature",
  .state_class="measurement"
};
ha.publishSensorDiscovery(temp);

// later:
ha.publishState("temperature", "22.5");
```

### Switch

```c++
HaSwitchConfig relay{
  .common = { .object_id="relay1", .name="Desk Light", .icon="mdi:lightbulb" }
};
ha.publishSwitchDiscovery(relay);

// later:
ha.publishStateSwitch("relay1", true); // ON
```

### Binary sensor

```c++
HaBinarySensorConfig motion{
  .common = { .object_id="motion", .name="Hallway Motion", .icon="mdi:motion-sensor" },
  .device_class = "motion"
};
ha.publishBinarySensorDiscovery(motion);

// later:
ha.publishState("motion", "ON");
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
