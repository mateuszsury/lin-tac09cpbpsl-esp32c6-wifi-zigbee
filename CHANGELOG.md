# Changelog

## 0.9.40 - 2026-08-20

- translated the embedded web dashboard, USB recovery prompts and Home
  Assistant discovery labels into English;
- removed the Polish retail phrase from the public README;
- added a repository-wide English-language regression test.

## 0.9.39 - 2026-08-19

- identified and documented the validated appliance as the black compact LIN
  TAC09CPBPSL;
- completed KAmodNTS0104PW four-channel production topology;
- moved main-RX output to ESP32-C6 GPIO0;
- added response-confirmed MQTT/HTTP command injection;
- confirmed power, setpoint, fan, mode, Quiet and unit controls on hardware;
- added MQTT Home Assistant discovery/dashboard and validated Zigbee2MQTT
  join, reporting and command support;
- made SNIFFER the safe default and reduced the public profile surface;
- removed Arduino/Nano and voltage-divider experiments;
- consolidated public documentation and credential handling.
