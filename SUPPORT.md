# Support

## Before opening an issue

1. Confirm the display and main-controller PCB identifiers.
2. Start from the `SNIFFER` profile.
3. Read [docs/HARDWARE.md](docs/HARDWARE.md) and
   [docs/INSTALLATION.md](docs/INSTALLATION.md).
4. Run the checks in [docs/VALIDATION.md](docs/VALIDATION.md).
5. Remove credentials, private IP addresses and unrelated Home Assistant data
   from every log or capture.

Use the bug-report template for reproducible firmware or integration failures.
Use the protocol-capture template for new UART evidence. Security issues belong
in a private [GitHub security advisory](https://github.com/mateuszsury/lin-tac09cpbpsl-esp32c6-wifi-zigbee/security/advisories/new).

## What maintainers need

- full source commit and firmware version;
- ESP32-C6 board and KAmod wiring;
- exact air-conditioner PCB identifiers;
- selected profile and MQTT or Zigbee transport;
- redacted logs with link ages and frame/error counters;
- expected behavior, observed behavior and repeatable steps.

Support cannot make an unverified 5 V or mains connection safe. If the board
identity, ground reference or signal voltage is uncertain, stop at `SNIFFER` and
measure the interface before enabling the translator.
