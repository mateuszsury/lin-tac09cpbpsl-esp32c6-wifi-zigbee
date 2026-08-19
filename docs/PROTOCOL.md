# TD display protocol

## Scope

This map applies to the observed `TD-YD-PSL-XS V1.0` display and
`810901123B` main controller. It is not the TCL split-unit CN16/USB protocol:
that separate family commonly uses a `BB` header and 9600-8E1.

## Framing

- UART: 9600 baud, 8 data bits, no parity, one stop bit;
- idle level: high;
- header: `54 44` (ASCII `TD`);
- panel -> main: 15 bytes, byte 2 is `0B`;
- main -> panel: 22 bytes, byte 2 is `12`;
- XOR of every complete frame equals `10`.

There is no evidence of encryption. The length byte describes the body; the
last wire byte is the checksum.

## Decoded fields

Indexes are zero-based.

| Index | Panel -> main | Main -> panel | Confidence |
|---:|---|---|---|
| 0..1 | `54 44` | `54 44` | confirmed header |
| 2 | `0B` | `12` | confirmed direction/length |
| 3 | `11` steady, `12` event | same | confirmed event marker |
| 4 | `31`, `33`, `22`, `43` | `31`, `33`, `21`, `43` | confirmed mode/fan enum |
| 5 bit `80` | power | power | confirmed |
| 5 bit `10` | Quiet | Quiet | confirmed |
| 5 bit `08` | Fahrenheit display | Fahrenheit display | confirmed |
| 5 bit `20` | Timer active | Timer active | read-only; duration unknown |
| 6 | retained Cool setpoint C | contextual display temperature C | confirmed behavior |
| 9 | rounded Fahrenheit setpoint | opaque | confirmed correlation |
| 11..12 | opaque | raw sensor/status values | meaning unknown |
| 14 | checksum | contextual Fahrenheit temperature | confirmed framing/context |
| 21 | - | checksum | confirmed framing |

Mode/fan values:

| Panel | Main acknowledgement | Meaning |
|---|---|---|
| `31` | `31` | Cool, Low fan |
| `33` | `33` | Cool, High fan |
| `22` | `21` | Fan mode |
| `43` | `43` | Dry mode |

Main bytes 6/14 are contextual. They match the target in Cool, but showed a
display/ambient value in Fan and Dry. The firmware therefore retains target
temperature from panel state and does not present these main bytes as a target
outside Cool.

## Command semantics

`MITM_NTS` replaces one complete panel frame at a frame boundary, regenerates
its checksum, then waits for a matching main-board acknowledgement. Until that
reply arrives, state remains non-optimistic. A physical panel event cancels the
pending override and wins. Timeout restores transparent forwarding.

Supported writes are power, Cool setpoint 18-32 C, Cool/Fan/Dry, Low/High fan,
Quiet and display units. Timer duration, Sleep and Swing are unknown. Timer is
parsed read-only because the main controller rejected a bit-only Timer write.

## Evidence rules

- Label one physical action at a time and repeat it at least three times.
- Preserve every unknown byte when mutating a frame.
- Do not name a field from a single transition.
- Do not claim a write until the main-board reply and appliance behavior both
  agree.
- Keep raw captures under ignored `captures/tmp/`; publish only redacted,
  action-labelled evidence under `captures/reference/`.

Curated examples are in `captures/reference/protocol-baseline-20260812.txt`,
`remote-button-sequence-20260812.txt` and
`isolated-features-20260812.txt`.
