# Home Assistant integration

## MQTT (recommended)

Configure the broker in ignored `main/klima_secrets_local.h`:

```c
#define KLIMA_MQTT_BROKER_URI "mqtt://homeassistant.local:1883"
#define KLIMA_MQTT_USERNAME "device-specific-user"
#define KLIMA_MQTT_PASSWORD "device-specific-password"
#define KLIMA_MQTT_DEVICE_ID "" /* derive klima_xxxxxx from STA MAC */
```

Create a broker account limited to this device's topics. The base topic is
`klima/<device_id>`. The firmware publishes retained availability/state and
Home Assistant MQTT Discovery, and republishes discovery after the Home
Assistant birth message.

The Climate entity exposes:

- Off, Cool, Fan-only and Dry;
- target temperature 18-32 C;
- Low and High fan modes;
- separate Quiet and Fahrenheit-display switches;
- link, protocol and command diagnostics.

Commands are non-optimistic. Retained command messages are ignored, values are
strictly validated and state changes only after a matching main-board reply.
Timer is read-only; Timer duration, Sleep and Swing are not exposed.

### Dashboard

`ha/klima_dashboard_section.yaml` is a reusable Sections-view block. Its entity
IDs assume the default MQTT device ID; adjust them after discovery if needed.
It uses built-in Thermostat/Tile cards plus optional Mushroom cards. Remove or
replace optional cards if Mushroom is not installed.

In Home Assistant:

1. open the target dashboard;
2. choose **Edit dashboard**;
3. open the raw configuration editor;
4. paste the object into the view's `sections` list;
5. replace entity IDs with those created by MQTT Discovery;
6. save and test desktop and mobile layouts.

Do not publish a complete dashboard export: it may contain personal entity
names, internal URLs and unrelated household configuration.

## Zigbee2MQTT

Build with `-DKLIMA_HA_TRANSPORT=ZIGBEE`, perform a full USB flash for the
Zigbee partition layout, and copy
`zigbee2mqtt/klima_wifi_converter.js` to Zigbee2MQTT's external-converters
directory. Restart Zigbee2MQTT and permit joining.

The ESP32-C6 acts as an always-powered Zigbee End Device, not a coordinator or
router. It exposes one thermostat endpoint, Fan Control and a manufacturer
cluster for verified controls/diagnostics. Wi-Fi STA remains available for
HTTP/OTA; fallback AP is disabled to reduce single-radio coexistence load.

The included End Device and converter have been validated for joining,
interview, state reporting and confirmed commands. A Zigbee OTA upgrade has not
yet completed the release gate, so perform the first installation over USB and
keep a wired recovery path.

## Automated appliance HIL

`tools/hil_validate.py` can exercise the confirmed command matrix and use a
Home Assistant power sensor as independent evidence. It requires:

```powershell
$env:HA_URL = 'http://homeassistant.local:8123'
$env:HA_TOKEN = '<long-lived-access-token>'
py -3 tools\hil_validate.py `
  --device .local\device.json `
  --power-entity sensor.<power_entity> `
  --output .local\hil-report.json
```

The runner refuses stale links, an initial ON state, unavailable control or
missing confirmation. It attempts a final OFF command after any partial run.
Review the final physical state; software cleanup is not an electrical safety
mechanism.
