# Acknowledgements

This project exists because several open projects make local device control
practical:

- [Espressif ESP-IDF](https://github.com/espressif/esp-idf) provides the
  ESP32-C6 runtime, networking, OTA and hardware drivers.
- [Espressif Zigbee SDK](https://github.com/espressif/esp-zigbee-sdk) provides
  the Zigbee stack and Home Automation device model.
- [Home Assistant](https://www.home-assistant.io/) provides the local smart-home
  platform and MQTT Discovery contract.
- [Zigbee2MQTT](https://www.zigbee2mqtt.io/) provides the coordinator bridge and
  external-converter interface used by the Zigbee build.
- [KAmod](https://kamami.pl/) produces the KAmodNTS0104PW translator used by the
  reference wiring.

Thanks also go to contributors who provide redacted captures, board identifiers
and repeatable hardware observations. Protocol findings should always credit
the original issue or pull request that supplied the evidence.

TCL, iFFALCON, LIN, Equation and other manufacturer or product names are used
only to identify potentially related hardware. This project is independent and
is not affiliated with, sponsored by or endorsed by those companies.
