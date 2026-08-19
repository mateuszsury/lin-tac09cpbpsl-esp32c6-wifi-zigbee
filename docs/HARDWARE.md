# Hardware and wiring

## Safety boundary

The main controller belongs to a mains-powered appliance. Disconnect mains and
allow stored energy to discharge before touching wiring. Do not leave USB, a
grounded computer or an earth-referenced oscilloscope attached to energized
electronics unless the low-voltage domain has independently been proven
isolated. Enclose the ESP32, translator and every joint before unattended use.

The reference air conditioner may use R290 refrigerant. Do not use flames,
heat guns or spark-producing work near the refrigerant circuit.

## Required parts

- ESP32-C6 development board;
- KAmodNTS0104PW four-channel bidirectional level translator;
- one 10 kOhm resistor for the OE pull-down;
- short insulated hookup wire, suitable connectors and an enclosure;
- multimeter for unpowered continuity and supply checks.

No Arduino, ADC input or resistor divider is part of the final design. There
are no bypass jumpers in normal operation.

## Power and enable

| KAmod pin | Connect to |
|---|---|
| `VDD(A)` | ESP32-C6 `3V3` |
| `VDD(B)` | regulated panel/main `+5V` |
| `GND(A)` | ESP32-C6 `GND` |
| `GND(B)` | panel/main low-voltage `GND` |
| `OE` | ESP32-C6 GPIO3 and 10 kOhm to GND |

Connect both ground pins even though the system has a common ground; they are
the local return paths for both translator sides. Never connect `VDD(A)` to
`VDD(B)`. The external pull-down keeps every channel high-impedance during
reset. Firmware raises OE only after bridge initialization in a profile that
requires outputs.

## Signal wiring

```text
                    KAmodNTS0104PW
                 +------------------+
ESP 3V3 ---------| VDD(A)    VDD(B) |--------- panel/main +5 V
ESP GND ---------| GND(A)    GND(B) |--------- panel/main GND
ESP GPIO3 -------| OE               |---10k--- GND
                 |                  |
main PCB TX -----| B1          A1   |--------- ESP GPIO4
ESP GPIO6 -------| A2          B2   |--------- panel PCB RX
panel PCB TX ----| B3          A3   |--------- ESP GPIO7
ESP GPIO0 -------| A4          B4   |--------- main PCB RX
                 +------------------+
```

The original four-wire display link must be split at TX and RX. Power and
ground remain common; each data direction passes through the ESP bridge. Do
not connect A1-A2, A3-A4, GPIO4-GPIO6 or GPIO0-GPIO7. Do not connect a B-side
signal directly to an ESP GPIO.

## Unpowered verification

Before applying power:

1. confirm every connection against the table above;
2. confirm approximately 10 kOhm between OE and GND;
3. confirm no continuity between 3.3 V and 5 V;
4. confirm no short from any signal to either supply or ground;
5. confirm both original TX lines lead only to B1/B3 and both RX lines only to
   B2/B4;
6. remove every temporary bypass jumper and isolate all unused wires.

## First powered verification

1. Flash `SNIFFER` while the ESP is disconnected from the appliance.
2. Disconnect USB and power the complete low-voltage assembly as intended.
3. Confirm the panel powers normally and the appliance remains controllable by
   its original controls only when the selected topology permits it.
4. Inspect `/api/status` and `/api/ac-state`: both links must become fresh and
   checksum/framing counters must remain zero.
5. Power down before changing to `BRIDGE` or `MITM_NTS`.

`SNIFFER` deliberately leaves OE low. `PANEL_DIAG` and `PANEL_BENCH` are only
for a detached display. Never attach `PANEL_BENCH` to the real main PCB.
