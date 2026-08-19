# Validation and evidence

## Current status

| Gate | Status |
|---|---|
| Python contract/unit tests | PASS |
| Native C protocol tests | PASS |
| Zigbee2MQTT converter syntax | PASS |
| ESP-IDF 5.3.2 MQTT SNIFFER/BRIDGE/MITM_NTS builds | PASS |
| ESP-IDF 5.3.2 Zigbee SNIFFER/BRIDGE/MITM_NTS builds | PASS |
| Complete appliance MQTT MITM control matrix | PASS |
| Physical panel priority and response confirmation | PASS |
| Real Zigbee2MQTT join/interview/reporting/commands | PASS |
| Zigbee OTA upgrade from Zigbee2MQTT | NOT_RUN |
| Long unattended mixed Wi-Fi/UART soak | NOT_RUN |

The physical MQTT acceptance covered power on/off, 18/19 C, both fan speeds,
Cool/Fan/Dry, Quiet and Celsius/Fahrenheit. Every successful request matched a
main-board reply, and checksum/framing/command-timeout counters stayed at zero.
Timer writes were rejected after the main controller proved that its active bit
alone was insufficient; the feature remains read-only.

The Zigbee acceptance used the ESP32-C6 End Device build and the included
external converter. It covered network joining, completed interview, thermostat
and fan reporting, diagnostic attributes, and confirmed command round trips.
The Wi-Fi/MQTT and Zigbee firmware images are separate builds and are validated
independently.

## Evidence levels

- **host-tested**: parser, command mutation or contract tested without an ESP;
- **build-tested**: exact firmware profile linked with the supported ESP-IDF;
- **bench-tested**: detached low-voltage hardware exercised;
- **appliance HIL-tested**: complete powered appliance and physical behavior;
- **NOT_RUN**: no evidence; never infer success from adjacent tests.

## Release acceptance

A release candidate should have:

1. a clean source commit and no tracked credentials;
2. passing host tests and converter syntax check;
3. successful profile/transport build matrix;
4. release manifest with SHA-256 for each distributed binary;
5. safe default `SNIFFER` behavior;
6. powered HIL for every active transport being advertised;
7. explicit `NOT_RUN` labels for remaining physical or authenticated gates.

`MITM_NTS` acceptance additionally requires fresh links in both directions,
growing frame counters, no new protocol errors, working physical controls,
response-confirmed commands and independently consistent appliance power.

## Reproducing software evidence

```powershell
py -3 -m pytest -q tests
node --check zigbee2mqtt\klima_wifi_converter.js
.\tools\build_profiles.ps1 -Profile SNIFFER -Transport MQTT -Clean -Manifest
.\tools\build_profiles.ps1 -Profile BRIDGE -Transport MQTT -Clean -Manifest
.\tools\build_profiles.ps1 -Profile MITM_NTS -Transport MQTT -Clean -Manifest
```

Repeat for `-Transport ZIGBEE`. Do not publish old manifests after source or
configuration changes; rebuild first.
