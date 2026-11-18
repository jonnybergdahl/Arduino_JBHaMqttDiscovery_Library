# Changelog

## 1.1.0 (2026-11-18)

- `value_template` on entities (`val_tpl`), so several entities can read their state from one JSON state topic.
- `unique_id` on entities, to keep the unique ID an entity already has.
- `publishStateTopic()`: publish and keep a state on any topic.
- Lights can use Home Assistant's default schema (`HaLightSchema::Default`), with separate state, brightness, RGB and color temperature topics. Commands are passed on as JSON, the same as for JSON schema lights.
- `onMessage()`: get incoming messages with their topic, also for your own topics added with `subscribe()`, which subscribes again after every reconnect.
- `setAvailabilityTopic()`: use another availability topic than `<base>/<node_id>/status`.
- `migrateEntityDiscovery()`: take over entities from single-component discovery topics, using Home Assistant's migration. Only entities with a matching unique ID are migrated. To know when the broker has sent all retained old configs, the library publishes a message to `<base>/<node_id>/migration_sync` and waits until it comes back (at most 10 s). Migrate messages and clears go out at QoS 0 (still retained), since AsyncMqttClient waits for every QoS 1 acknowledgement before sending anything else; a lost one is handled again on the next connect.

### Changed

- `publishState()` now publishes on the entity's `state_topic_override` if it has one. Before, it always used the default `<base>/<node_id>/<object_id>/state`, which Home Assistant wasn't listening to for those entities. If your firmware relied on that, publish there with `publishStateTopic()`.
- `publishAvailabilityOnline()` returns whether the transport accepted the publish. `removeDevice()` returns `true` whenever a device is set, also when the publish is retried later.

### Fixed

- The AsyncMqttClient and espMqttClient transports dropped incoming messages that the client hands over in parts, which happens for payloads over a few hundred bytes. They now put them together, up to `setMaxMessageSize()` bytes (4096 by default).
- When the transport didn't accept the device payload, a removal stub or availability (for example espMqttClient on ESP8266 when short of memory), they were never published again. `tick()` now retries after 1 s, doubling up to 30 s, and holds states until the device payload is out. Migrate messages and the clearing of migrated topics are retried too, and the device payload waits until all migrate messages are out.
- The espMqttClient transport on ESP8266 logs the largest free block and `EMC_MIN_FREE_MEMORY` when a publish fails. The README now says to lower `EMC_MIN_FREE_MEMORY` on ESP8266.

## 1.0.0 (2026-11-18)

First release.

- Home Assistant MQTT **device discovery**: one retained payload per device on `homeassistant/device/<node_id>/config`, holding all its entities.
- Entity types: sensor, binary sensor, switch, button, number, select, text, light (JSON schema: on/off, brightness, color temperature, RGB) and cover (with optional position).
- Entity options: icon, entity ID (`def_ent_id`), `name: null` for the device's main feature, entity category (config/diagnostic), `expire_after`, and per-entity state, command and availability topics.
- Device info with an optional "Visit" link (`configuration_url`), and an origin block (`o`).
- Commands from Home Assistant go to one `onCommand()` handler, always called from `loop()`.
- Availability with an MQTT Last Will, so entities go unavailable when the device drops off.
- The device payload, availability and last states are sent again after every reconnect and after Home Assistant restarts (birth message, with a random delay).
- Entities and states published while disconnected are kept and sent on connect.
- Entity and device removal.
- MQTT transports for **espMqttClient**, **PubSubClient** and **AsyncMqttClient**, all with automatic reconnect (exponential backoff) and `reconnectNow()` for when Wi-Fi comes back.
- Accepts `const char*`, `std::string` and Arduino `String`.
- Tested on ESP8266 (Wemos D1 mini) and ESP32-S3 (Unexpected Maker TinyS3) against Home Assistant 2026.11.
