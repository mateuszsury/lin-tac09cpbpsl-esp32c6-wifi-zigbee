# Klima WiFi AC Bridge — Zigbee2MQTT

`klima_wifi_converter.js` is an external Zigbee2MQTT converter for the single
endpoint ESP32-C6 device. Copy it to the Zigbee2MQTT `data` directory and add
it under **Settings → External converters** (or `external_converters` in the
configuration), then re-interview the device.

The converter exposes one Home Assistant climate entity with `off`, `cool`,
`fan_only` and `dry` modes, a cooling setpoint from 18–32 °C, and `low` /
`high` fan modes. It also exposes read-only link, bridge, frame and
sensor diagnostics from manufacturer cluster `0xFC10` (manufacturer `0x131B`).

Every command is guarded by the reported `control_available` diagnostic. The
converter writes only standard Thermostat/FanControl attributes and never
publishes an optimistic state; the final state comes from a verified AC frame.
The firmware's PASSIVE profile therefore fails closed while jumpers remain
attached. No OTA, timer, sleep or swing controls are defined.
