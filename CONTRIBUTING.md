# Contributing

Contributions are welcome, especially labelled captures from the exact
`TD-YD-PSL-XS` / `810901115C` display and `810901123B` controller pair.

## Safety boundary

Do not test a powered mains appliance while the ESP is connected to USB or an
earth-referenced computer. Follow [docs/HARDWARE.md](docs/HARDWARE.md). Never
publish Wi-Fi, MQTT, Home Assistant or device-token credentials.

The default contribution target is `SNIFFER` (with `PASSIVE` retained as an
alias). A change must not make SNIFFER/PASSIVE
drive GPIO4/5/6/7, subscribe to actuator topics or accept control requests.
Active MITM results must include both-direction frame evidence, explicit
main-board confirmation and the final appliance power measurement.

## Evidence rules

- Keep raw experimental captures under ignored `captures/tmp/`.
- Curate non-sensitive, action-labelled examples under `captures/reference/`.
- Do not assign a semantic field name from a single ambiguous transition.
- Preserve unknown bytes and frames; never silently normalize them.
- State whether a claim is host-tested, build-tested, physically HIL-tested or
  still blocked.

## Local checks

Run before opening a pull request:

```powershell
py -3 -m pytest -q tests
node --check zigbee2mqtt\klima_wifi_converter.js
```

From an initialized WSL/Linux checkout, run the native protocol test:

```bash
cmake -S tests -B build/host-tests
cmake --build build/host-tests
ctest --test-dir build/host-tests --output-on-failure
```

Build `SNIFFER`, `BRIDGE` and (only for a gated HIL branch) `MITM_NTS` for the
transport you changed. CI builds the full canonical profile x MQTT/Zigbee
matrix with ESP-IDF 5.3.2. A new board, translator or protocol variant must
remain marked `physical_hil=NOT_RUN` until it is tested on the real appliance.

Use focused commits and update `docs/PROTOCOL.md`, `docs/VALIDATION.md` and the
relevant contract tests whenever the wire or integration contract changes.

Contributors must follow [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md). Third-party
code requires a compatible license, preserved notices and a corresponding
entry in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
