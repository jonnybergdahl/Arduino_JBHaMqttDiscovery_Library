# JBHaMqttDiscovery
[![CI](https://github.com/jonnybergdahl/Arduino_JBHaMqttDiscovery/actions/workflows/ci.yml/badge.svg)](https://github.com/jonnybergdahl/Arduino_JBHaMqttDiscovery/actions/workflows/ci.yml)

An Arduino library for Home Assistant MQTT Discovery on ESP8266 and ESP32.

## What is this library?
**JBHaMqttDiscovery** makes sensors and devices connected to ESP8266 and ESP32 boards show up automatically in **Home Assistant** via **MQTT Discovery**.

It builds a Home Assistant *device discovery* payload with all your entities (sensors, binary sensors, switches, buttons, numbers, selects, texts, lights and covers) and publishes it as one **retained** message, so Home Assistant creates the device and its entities without manual UI/YAML setup.
It also handles availability, commands from Home Assistant, reconnects and Home Assistant restarts.

It is also **transport-agnostic**: you can use different MQTT client libraries through a small “transport” adapter (PubSubClient, espMqttClient or AsyncMqttClient), while keeping the same discovery API in your firmware.

## Installation

### Arduino IDE

1. Open **Sketch** > **Include Library** > **Manage Libraries...**.
2. Search for **JBHaMqttDiscovery** and select **Install**. Accept installing the dependencies (ArduinoJson and JBLogger).
3. Install one MQTT client the same way: **espMqttClient** (recommended), **PubSubClient** or **AsyncMqttClient**.

### PlatformIO

Add the library and one MQTT client to `platformio.ini`:

```ini
lib_deps =
    jonnybergdahl/JBHaMqttDiscovery@^1.2.0
    bertmelis/espMqttClient
```

> The library isn't in the PlatformIO registry yet. Until it is, install it straight from GitHub instead:
>
> ```ini
> lib_deps =
>     https://github.com/jonnybergdahl/Arduino_JBHaMqttDiscovery.git#1.2.0
>     bertmelis/espMqttClient
> ```

The examples are in `examples/` and under **File** > **Examples** > **JBHaMqttDiscovery** in the Arduino IDE.

## Usage

### Dependencies

1. [ArduinoJson](https://arduinojson.org/)
2. One MQTT client: [PubSubClient](https://pubsubclient.knolleary.net/), [espMqttClient](https://github.com/bertmelis/espMqttClient) or [AsyncMqttClient](https://github.com/marvinroger/async-mqtt-client)
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

### Which MQTT client?

| Client | Notes |
|---|---|
| **espMqttClient** | Maintained, the best choice for new projects. Sends large payloads in chunks, so it needs the least memory. ESP8266 and ESP32. |
| **PubSubClient** | Simple and widely used. Needs a buffer as large as the whole discovery payload, and reconnect attempts block `loop()`. |
| **AsyncMqttClient** | Not maintained since 2019. Supported for existing projects; the transport works around its connect bug. |

All three behave the same through the library: the same API, auto-reconnect, Last Will and commands.

### Running without a computer

Devices usually run from a power supply, with nothing connected to the USB port. So don't wait for the serial port in `setup()`: `while (!Serial) {}` never ends on boards with native USB. The examples wait at most 2 seconds:

```c++
Serial.begin(115200);
unsigned long serialStart = millis();
while (!Serial && millis() - serialStart < 2000) {
  delay(10);
}
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
// ESP32 native USB (USB-Serial/JTAG, e.g. ESP32-S3/C3): drop log output when no
// computer reads it. Otherwise each log line can block, and loop() slows to a crawl.
Serial.setTxTimeoutMs(0);
#endif
```

### Arduino String

All functions that take text also accept Arduino `String` (and `std::string`), e.g. `ha.publishState("temperature", String(t, 1))` or `transport.connect(nodeId)`.
The config structs (`HaDeviceInfo`, `HaSensorConfig`, ...) take `const char*`; pass `myString.c_str()`. The library copies the text, so local Strings are fine.

### espMqttClient (recommended)

```c++
#include <espMqttClient.h>
#include <HaDiscovery.h>
#include <transport/EspMqttClientTransport.h>

espMqttClient mqtt;
EspMqttClientTransport<espMqttClient> transport(mqtt);
HaDiscovery ha(transport);

void setup() {
  // ... Wi-Fi, ha.setDevice(...), ha.onCommand(...)
  transport.setServer("mqtt-broker.local", 1883, "user", "pass");
  transport.connect(nodeId.c_str());
  // ... publish*Discovery(...)
}

void loop() {
  ha.tick(); // REQUIRED: runs the client, publishes discovery, handles commands, reconnects
}
```

`EspMqttClientTransport` works with `espMqttClient`, `espMqttClientSecure` (TLS) and `espMqttClientAsync`; pass the class as the template parameter.
Who runs the client's `loop()` depends on the client:

- `espMqttClient` on **ESP8266**: `ha.tick()` does. That's the default there.
- `espMqttClient` on **ESP32**: its own task does, so `ha.tick()` must not. That's the default there. If you create it with `espMqttClientTypes::UseInternalTask::NO`, use `EspMqttClientTransport<espMqttClient> transport(mqtt, true);`.
- `espMqttClientAsync`: its network callbacks do. On ESP8266, use `EspMqttClientTransport<espMqttClientAsync> transport(mqtt, false);`.

> The transport takes over espMqttClient's `onConnect`, `onDisconnect` and `onMessage` callbacks.
> espMqttClient refuses to publish while the largest free block of heap is below `EMC_MIN_FREE_MEMORY` (16 KB by default); the transport logs it as a failed publish.
> An ESP8266 seldom has 16 KB in one block, so lower it with a build flag, for example `-D EMC_MIN_FREE_MEMORY=4096` in `build_flags` in PlatformIO.
> Keep the largest free block bigger than the device payload, which is sent in one piece.

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
States published before that are sent 2 seconds after it.

Home Assistant creates the entities from the device payload in the background, and only then subscribes to their state topics.
States sent right after the payload are lost on a new device (or one moved over with `migrateEntityDiscovery()`), and its entities show `unknown` until the next change.
So after every device payload the library holds the stored states for 2 seconds; states published in the meantime are kept and go out with them.
Change the delay with `ha.setStateDelay(ms)`, or turn it off with `ha.setStateDelay(0)`.

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

If the MQTT client doesn't accept the device payload or `online` (for example when it's short of memory), the library tries again from `ha.tick()` after 1 second, doubling up to 30 seconds, and holds the states until it gets through.

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

### Raw messages and your own topics

If your firmware already handles its own MQTT commands, or listens on topics that don't belong to an entity, use `onMessage()`.
It gets the topic and the payload as they arrived, for entity commands (before `onCommand()`) and for topics added with `subscribe()`:

```c++
ha.subscribe("myfirmware/office/ota/#");   // subscribed again after every reconnect
ha.onMessage([](const char* topic, const char* payload) {
  handleMyMessage(topic, payload);
});
```

The transport takes over the MQTT client's message callback, so subscribe through `ha.subscribe()`, not the client.
Don't let a subscription overlap the entities' command topics: some brokers then deliver a message once per matching subscription.

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

### Shared state topics and unique IDs

Several entities can read their state from one JSON message. Point them at the same topic with `state_topic_override`, pick the value with `value_template`, and publish the JSON with `publishStateTopic()`:

```c++
HaSensorConfig rssi{
  .common = { .object_id="rssi", .name="RSSI",
              .state_topic_override="office/diagnostics",
              .value_template="{{ value_json.rssi }}" }
};
ha.publishSensorDiscovery(rssi);

ha.publishStateTopic("office/diagnostics", "{\"rssi\":-52,\"ram\":201728}");
```

Like `publishState()`, `publishStateTopic()` keeps the last payload and sends it again on connect and after a Home Assistant restart.
`publishState()` also uses the entity's `state_topic_override` when it has one.

The unique ID is `<node_id>_<object_id>`. Set `.unique_id` to keep the unique ID an entity already has, e.g. when moving to this library from other discovery code; see [Moving from single-component discovery](#moving-from-single-component-discovery).

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

Uses Home Assistant's JSON schema by default, so all light commands arrive on one topic as JSON.
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

With `.schema = HaLightSchema::Default`, the light uses Home Assistant's default schema instead, with separate topics: `.../state` and `.../set` for on/off, `.../brightness` and `.../brightness/set`, `.../rgb` and `.../rgb/set` (`r,g,b`), and `.../color_temp` and `.../color_temp/set` (kelvin).
Use it when the firmware already has those topics; each one can be changed with the `*_topic_override` fields.
The library turns the commands into the same JSON as above, so `parseLightCommand()` and `publishStateLight()` work the same way.

```c++
HaLightConfig display{
  .common = { .object_id="display", .name="Display" },
  .brightness_scale=100,
  .schema=HaLightSchema::Default
};
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

## Other availability topic

Availability is on `<base>/<node_id>/status` by default. To use another topic, e.g. one your firmware already has, call `setAvailabilityTopic()` before connecting:

```c++
ha.setAvailabilityTopic("myfirmware/office/availability");
```

It's used for the Last Will, for `online`/`offline` and in the device payload.

## Moving from single-component discovery

Older firmware often publishes one discovery message per entity, on `homeassistant/<component>/<node_id>/<object_id>/config`.
If those are still retained on the broker, Home Assistant keeps those entities, and ignores entities in the device payload with the same unique ID.

Call `ha.migrateEntityDiscovery()` before connecting, and give the entities the unique IDs they had (`.unique_id`).
After every connect, the library then waits for the retained messages on `homeassistant/+/<node_id>/+/config`, and follows Home Assistant's migration steps for each one.
To know when they're all in, it also publishes a message to `<base>/<node_id>/migration_sync`, which it subscribes to as well. The broker sends that back after the retained messages. This usually takes well under a second; the library gives up waiting after 10 seconds.

1. Publishes `{"migrate_discovery": true}` to the old topic. Home Assistant unloads the entity, but keeps its settings.
2. Publishes the device payload, which loads the entity again.
3. Clears the old topic 5 seconds later.

The entities keep their entity IDs, history and settings. Once the old topics are cleared, nothing more happens, so it's fine to leave it on; the only cost is the short wait after each connect.

Only old configs whose unique ID matches an entity in the device payload are migrated, so add the entities before connecting.
The others are left alone, with a warning in the log: Home Assistant then shows the old entity next to the new one, which you can fix by correcting `.unique_id`.
Migrating them would unload the old entity with nothing to load it again.

> Check the unique IDs against the old firmware before flashing a real device, e.g. the `uniq_id` in the old discovery payloads on the broker.

## Using a client your firmware already connects

With PubSubClient, the library can work with a client your firmware already sets up, connects and reconnects itself, e.g. when adding the library to existing firmware:

```c++
PubSubClientTransport transport(mqtt);   // mqtt: your PubSubClient
HaDiscovery ha(transport, "homeassistant", "myfirmware");

transport.setAutoReconnect(false);                   // your firmware reconnects
ha.setAvailabilityTopic("myfirmware/office/status"); // the topic of your Last Will
ha.setDevice({ ... });
ha.onMessage(myMessageHandler);                      // the transport takes over the client's callback
```

Then call `mqtt.loop()` and `ha.tick()` from `loop()`; `tick()` notices when your firmware has connected.
Since the transport's `setServer()` and `connect()` aren't used:

- The Last Will set by `setDevice()` isn't sent. Connect with your own will on the same topic as `setAvailabilityTopic()`, with the payload `offline`, retained.
- The buffer isn't raised to 2048 bytes up front. It still grows when a publish needs more, but incoming messages must fit the size your firmware set with `mqtt.setBufferSize()`.
- Subscribe through `ha.subscribe()`, not the client, so the subscriptions come back after a reconnect.

