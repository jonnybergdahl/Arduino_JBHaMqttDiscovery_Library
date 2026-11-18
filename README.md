# JBHaMqttDiscovery
An Arduino library for Home Assistant MQTT Auto Discovery.

 > This is work in progress!

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
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String nodeId = "esp8266_" + mac;

  ha.setDevice({
    .node_id = nodeId.c_str(),
    .name = "Office Node",
    .manufacturer = "YourBrand",
    .model = "ESP8266",
    .sw_version = "1.0.0",
    .identifiers = mac.c_str()
  });

  // Configure mqtt server/credentials through the transport, then connect
  // through the transport so the Last Will set by setDevice() is used:
  transport.setServer("mqtt-broker.local", 1883, "user", "pass");
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
  mqtt.loop(); // REQUIRED
  ha.tick();   // REQUIRED: detects (re)connects and Home Assistant restarts
}
```

> PubSubClient's default 256 byte buffer is too small for discovery payloads, which are usually 400–600 bytes.
> `PubSubClientTransport` raises it to 1024 bytes in `setServer()`, and grows it further if a publish needs more.
> Pass a different minimum as the second constructor argument, e.g. `PubSubClientTransport transport(mqtt, 2048);`.

### AsyncMqttClient (event-driven, no polling required)
```c++
#include <AsyncMqttClient.h>
#include <HaDiscovery.h>
#include <transport/AsyncMqttClientTransport.h>

AsyncMqttClient mqtt;
AsyncMqttClientTransport transport(mqtt);
HaDiscovery ha(transport);

void setup() {
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

  // Configure mqtt server/credentials through the transport, then connect
  // through the transport so the Last Will set by setDevice() is used:
  transport.setServer("mqtt-broker.local", 1883, "user", "pass");
  transport.connect(nodeId.c_str());

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
  ha.tick(); // Re-publishes discovery after Home Assistant restarts
}
```

## Availability and Last Will

`setDevice()` sets the MQTT Last Will to `offline` on the availability topic (`<base>/<node_id>/status`, retained).
When the connection comes up, the library publishes `online` there.
If the device loses power or network, the broker publishes `offline` and Home Assistant marks its entities unavailable.

Call `setDevice()` before connecting, and connect through the transport (`transport.connect(...)`), not the MQTT client, so the will is sent.

## Reconnects and Home Assistant restarts

The library keeps every discovery config you publish, and the last state of each entity.
It sends them again:

- every time the MQTT connection comes up, and
- when Home Assistant comes back online (it publishes `online` on `homeassistant/status`), after a random delay of up to 5 seconds so many devices don't publish at once. Change it with `ha.setRepublishDelay(ms)`.

Discovery configs and states published while disconnected are kept and sent when the connection comes up.
So you can publish them in `setup()` right after `connect()`, even with AsyncMqttClient, which connects in the background.

Call `ha.republishAll()` to send everything again yourself.

> Each entity's config is kept in RAM (about 400–600 bytes each).

## Commands from Home Assistant

Switches and buttons get a command topic (`<base>/<node_id>/<object_id>/set` by default).
The library subscribes to it, again after every reconnect, and passes incoming commands to one handler:

```c++
void onCommand(const char* objectId, const char* payload) {
  if (strcmp(objectId, "relay1") == 0) {
    bool on = strcmp(payload, "ON") == 0;
    digitalWrite(RELAY_PIN, on ? HIGH : LOW);
    ha.publishStateSwitch("relay1", on); // report the new state back
  }
  if (strcmp(objectId, "restart") == 0) {
    ESP.restart();
  }
}

void setup() {
  // ...
  ha.onCommand(onCommand);
}
```

> With AsyncMqttClient the handler runs in the network task, not in `loop()`. Keep it short and hand longer work over to `loop()`.

## Entity names and IDs

Home Assistant shows entities with the device name in front.
So give entities short names without the device name: `"Temperature"` on device `"Office Node"` shows as **Office Node Temperature**.

- `.use_device_name = true` sends `"name": null`. The entity is then the device's main feature and shows with just the device name, e.g. the relay of a smart plug.
- `.entity_id = "office_temperature"` asks Home Assistant for the entity ID `sensor.office_temperature` (sent as `def_ent_id`, the domain is added for you). Home Assistant only uses it when it first creates the entity. Without it, Home Assistant makes the ID from the device and entity names, e.g. `sensor.office_node_temperature`.
- `object_id` only sets the MQTT topics and the unique ID. It doesn't affect the entity ID.

```c++
HaSwitchConfig plug{
  .common = { .object_id="relay", .use_device_name=true }
};
HaSensorConfig temp{
  .common = { .object_id="temperature", .name="Temperature", .entity_id="office_temperature" },
  .unit_of_measurement="°C",
  .device_class="temperature"
};
```

## Origin

Every discovery config includes an origin block (`o`) with this library's name, version and URL.
Home Assistant logs it when entities are discovered, which helps when tracking down where an entity came from.
To send your firmware's details instead, call this before publishing discovery configs:

```c++
ha.setOrigin("MyFirmware", "2.1.0", "https://example.com/support");
```

## Entity usage

### Sensor

```c++
HaSensorConfig temp{
  .common = { .object_id="temperature", .name="Temperature", .icon="mdi:thermometer" },
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
  .common = { .object_id="relay1", .name="Light", .icon="mdi:lightbulb" }
};
ha.publishSwitchDiscovery(relay);

// "ON"/"OFF" commands arrive in the onCommand() handler.
// Report the state:
ha.publishStateSwitch("relay1", true); // ON
```

### Binary sensor

```c++
HaBinarySensorConfig motion{
  .common = { .object_id="motion", .name="Motion", .icon="mdi:motion-sensor" },
  .device_class = "motion"
};
ha.publishBinarySensorDiscovery(motion);

// later:
ha.publishState("motion", "ON");
```

### Button

```c++
HaButtonConfig restart{
  .common = { .object_id="restart", .name="Restart", .icon="mdi:restart" },
  .payload_press = "PRESS"
};
ha.publishButtonDiscovery(restart);

// Presses arrive in the onCommand() handler with the "PRESS" payload.
```
