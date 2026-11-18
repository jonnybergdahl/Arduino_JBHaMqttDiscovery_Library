# JBHaMqttDiscovery
An Arduino library for Home Assistant MQTT Auto Discovery.

 > This is work in progress!

## What is this library?
**JBHaMqttDiscovery** makes sensors and devices connected to Arduino/ESP8266/ESP32 devices show up automatically in **Home Assistant** via **MQTT Discovery**.

It builds a Home Assistant *device discovery* payload with all your entities (sensors, switches, binary sensors, buttons) and publishes it as one **retained** message, so Home Assistant creates the device and its entities without manual UI/YAML setup.
It also handles availability, commands from Home Assistant, reconnects and Home Assistant restarts.

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
  ha.tick();   // REQUIRED: publishes discovery, handles commands, reconnects
}
```

> PubSubClient's default 256 byte buffer is too small for the discovery payload, which is about 400 bytes plus 150–250 bytes per entity.
> `PubSubClientTransport` raises it to 2048 bytes in `setServer()`, and grows it further if a publish needs more.
> Pass a different minimum as the second constructor argument, e.g. `PubSubClientTransport transport(mqtt, 4096);`.

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
  ha.tick(); // REQUIRED: publishes discovery, handles commands, reconnects
}
```

## How discovery is published

The library uses Home Assistant's device discovery: one retained payload on `homeassistant/device/<node_id>/config`, with the device info (`dev`), the origin (`o`), the shared availability topic and every entity as a component (`cmps`).

`publishSensorDiscovery()` and the other `publish*Discovery()` calls add or update an entity.
The payload is published from the next `ha.tick()`, so adding several entities in `setup()` sends one message.
States published before that are sent right after it.

> `node_id` is part of the topic, so it may only contain letters, digits, `_` and `-`.

## Availability and Last Will

`setDevice()` sets the MQTT Last Will to `offline` on the availability topic (`<base>/<node_id>/status`, retained).
When the connection comes up, the library publishes `online` there.
If the device loses power or network, the broker publishes `offline` and Home Assistant marks its entities unavailable.

Call `setDevice()` before connecting, and connect through the transport (`transport.connect(...)`), not the MQTT client, so the will is sent.

## Reconnects and Home Assistant restarts

After the first `transport.connect(...)`, the transport reconnects by itself from `ha.tick()` when the connection drops.
It tries again right away, then waits 1 second, doubling after each failed attempt up to 60 seconds.
Change the delays with `transport.setAutoReconnect(true, minMs, maxMs)`, or turn it off with `transport.setAutoReconnect(false)` if your firmware already reconnects itself.

> With PubSubClient each attempt blocks `loop()` until the broker answers or the attempt times out.

The transport doesn't know when Wi-Fi comes back, so after an outage it waits for the next scheduled attempt (up to 60 seconds).
Call `transport.reconnectNow()` from the Wi-Fi "got IP" event to reconnect right away; it's safe to call from the event callback:

```c++
// ESP32
WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) { transport.reconnectNow(); }, ARDUINO_EVENT_WIFI_STA_GOT_IP);

// ESP8266 (keep the handler, or the event is unregistered)
WiFiEventHandler gotIpHandler = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP&) { transport.reconnectNow(); });
```

The library keeps your entities and the last state of each one, and sends them again:

- every time the MQTT connection comes up, and
- when Home Assistant comes back online (it publishes `online` on `homeassistant/status`), after a random delay of up to 5 seconds so many devices don't publish at once. Change it with `ha.setRepublishDelay(ms)`.

Entities and states published while disconnected are kept and sent when the connection comes up.
So you can publish them in `setup()` right after `connect()`, even with AsyncMqttClient, which connects in the background.

Call `ha.republishAll()` to send everything again yourself.

> Each entity is kept in RAM (about 150–250 bytes each).
> With PubSubClient the transport's buffer also grows to fit the whole device payload (about 3.7 KB for 14 entities).
> On an ESP8266 that's about 6 KB of heap for 14 entities; keep an eye on free heap if you add many more.

## Commands from Home Assistant

Switches, buttons, numbers, selects, texts, lights and covers get a command topic (`<base>/<node_id>/<object_id>/set` by default; covers with a position also get `.../set_position`).
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

Commands are queued when they arrive and the handler is called from `ha.tick()`, so it always runs in `loop()`, with both MQTT clients.

## Device info

`setDevice()` takes the device details Home Assistant shows on the device page:

```c++
ha.setDevice({
  .node_id = nodeId.c_str(),            // required, letters, digits, '_' and '-'
  .name = "Office Node",
  .manufacturer = "YourBrand",
  .model = "ESP32",
  .sw_version = "1.0.0",
  .identifiers = mac.c_str(),           // stable id, defaults to node_id
  .configuration_url = "http://192.168.1.50/"  // optional "Visit" link
});
```

`configuration_url` adds a **Visit** link on the device page, e.g. to the device's own web interface.
It must be an `http://` or `https://` URL, or a `homeassistant://` path.
The strings are copied, so they can be local variables.

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

## Entity category and expiry

- `.entity_category = HaEntityCategory::Diagnostic` shows read-only device info (signal strength, uptime, free memory) under **Diagnostic** on the device page instead of with the main controls.
- `.entity_category = HaEntityCategory::Config` shows a setting under **Configuration**. Home Assistant doesn't allow it on sensors and binary sensors, so the library leaves it out there and logs a warning.
- `.expire_after = 300` (sensors and binary sensors) makes Home Assistant show the entity as unavailable if no new state arrives within 300 seconds.

> Home Assistant only picks up a changed `entity_category` on an existing entity when it creates the entity again, after a restart or a reload of the MQTT integration.

```c++
HaSensorConfig rssi{
  .common = { .object_id="rssi", .name="Wi-Fi signal", .entity_category=HaEntityCategory::Diagnostic },
  .unit_of_measurement="dBm",
  .device_class="signal_strength",
  .expire_after=120
};
```

## Origin

The discovery payload includes an origin block (`o`) with this library's name, version and URL.
Home Assistant logs it when entities are discovered, which helps when tracking down where an entity came from.
To send your firmware's details instead, call:

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


### Number

A value that can be set from Home Assistant, shown as a slider or an input box.

```c++
HaNumberConfig interval{
  .common = { .object_id="interval", .name="Report interval", .entity_category=HaEntityCategory::Config },
  .min=5, .max=300, .step=5,
  .mode=HaNumberMode::Slider,
  .unit_of_measurement="s"
};
ha.publishNumberDiscovery(interval);

// The new value arrives in onCommand() as text, e.g. "60". Report it back:
ha.publishStateNumber("interval", 60, 0);
```

### Select

A choice from a fixed list.

```c++
HaSelectConfig mode{
  .common = { .object_id="mode", .name="Mode" },
  .options = {"Eco", "Normal", "Turbo"}
};
ha.publishSelectDiscovery(mode);

// The chosen option arrives in onCommand(), e.g. "Eco". Report it back:
ha.publishState("mode", "Eco");
```

### Text

A string that can be set from Home Assistant.

```c++
HaTextConfig message{
  .common = { .object_id="message", .name="Display text" },
  .max_length=32
};
ha.publishTextDiscovery(message);

// The new text arrives in onCommand(). Report it back:
ha.publishState("message", "Hello");
```

### Light

Uses Home Assistant's JSON schema, so all light commands arrive on one topic as JSON.
`color_mode` is `OnOff`, `Brightness` (default), `ColorTemp` (kelvin) or `Rgb`.

```c++
HaLightConfig lamp{
  .common = { .object_id="lamp", .name="Lamp" },
  .color_mode=HaLightColorMode::Rgb
};
ha.publishLightDiscovery(lamp);

void onCommand(const char* objectId, const char* payload) {
  if (strcmp(objectId, "lamp") == 0) {
    HaLightCommand cmd;
    if (HaDiscovery::parseLightCommand(payload, cmd)) {
      if (cmd.has_state) lampOn = cmd.on;
      if (cmd.has_brightness) lampBrightness = cmd.brightness;
      if (cmd.has_color) { lampR = cmd.r; lampG = cmd.g; lampB = cmd.b; }
      // ... drive the LEDs, then report the state:
      HaLightState st;
      st.on = lampOn;
      st.brightness = lampBrightness;
      st.r = lampR; st.g = lampG; st.b = lampB;
      ha.publishStateLight("lamp", st);
    }
  }
}
```

### Cover

Blinds, shutters, garage doors and so on.

```c++
HaCoverConfig blind{
  .common = { .object_id="blind", .name="Blind" },
  .device_class="blind",
  .has_position=true
};
ha.publishCoverDiscovery(blind);

// onCommand() gets "OPEN", "CLOSE" or "STOP", or a position "0".."100" (0 = closed).
// Report the state ("open", "opening", "closed", "closing" or "stopped") and position:
ha.publishState("blind", "opening");
ha.publishStatePosition("blind", 40);
```

Set `.payload_stop = ""` for a cover that can't stop.

## Removing entities

```c++
ha.removeEntity("sensor", "temperature"); // one entity
ha.removeDevice();                        // the device and all its entities
```

`removeEntity()` publishes the device payload twice from the next `ha.tick()`: once with the entity as a removal stub (`{"p": "sensor"}`), then without it.
That removes it on current Home Assistant versions, and on later versions that treat the payload as the device's full set of entities.
Removing the last entity also clears the device discovery topic.

`removeDevice()` publishes an empty retained payload on the device discovery topic, which removes the device and all its entities.

> Deleting an MQTT entity in Home Assistant doesn't stop the device from announcing it, so it comes back. Remove it in the firmware instead.
