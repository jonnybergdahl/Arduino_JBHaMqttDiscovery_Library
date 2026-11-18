# Changelog

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
