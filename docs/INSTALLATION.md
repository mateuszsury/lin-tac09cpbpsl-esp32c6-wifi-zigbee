# Installation, build, flash and recovery

## 1. Prerequisites

- Git;
- Python 3.10 or newer;
- ESP-IDF 5.3.2 with ESP32-C6 support;
- Node.js for the Zigbee2MQTT converter syntax check;
- a data-capable USB cable for the first flash.

Clone the repository and initialize ESP-IDF in the current shell. On Linux:

```bash
git clone <repository-url> klima-wifi
cd klima-wifi
. "$IDF_PATH/export.sh"
```

On Windows, native ESP-IDF PowerShell works directly. The included PowerShell
helpers can also call ESP-IDF inside WSL. Set the WSL path once per shell:

```powershell
$env:KLIMA_WSL_IDF_EXPORT = '/home/<user>/esp/esp-idf/export.sh'
```

The path is intentionally not hard-coded in the repository.

## 2. Private configuration

Create the ignored local header:

```powershell
Copy-Item main\klima_secrets.example.h main\klima_secrets_local.h
```

Set Wi-Fi, a unique device token and optional MQTT credentials. Generate a
token rather than reusing a password:

```powershell
py -3 -c "import secrets; print(secrets.token_urlsafe(32))"
```

The device token must contain at least 24 characters. A missing/short token
disables all mutating HTTP endpoints. A fallback AP starts only when
`KLIMA_AP_PASSWORD` is 12-63 characters; otherwise the build remains STA-only.
MQTT remains disabled when its broker URI is empty.

Never put credentials in `klima_secrets.h`, CMake files, documentation,
captures or issue reports.

For host tools, copy the private device descriptor:

```powershell
New-Item -ItemType Directory .local -Force
Copy-Item tools\device.example.json .local\device.json
```

Edit `.local/device.json` with the device base URL and the same token. `.local`
is ignored by Git.

## 3. Build

Always begin with the safe profile:

```powershell
.\tools\build_profiles.ps1 `
  -Profile SNIFFER -Transport MQTT -Clean -Manifest
```

Direct ESP-IDF equivalent:

```bash
idf.py -B build/sniffer-mqtt \
  -DKLIMA_BRIDGE_MODE=SNIFFER \
  -DKLIMA_HA_TRANSPORT=MQTT build
```

After electrical and bidirectional-link validation, build the active image:

```powershell
.\tools\build_profiles.ps1 `
  -Profile MITM_NTS -Transport MQTT -Clean -Manifest
```

The manifest records the version, profile, transport, image size and SHA-256.
For a reproducible release, keep the source commit and manifest together.

## 4. First USB flash

The first installation must flash the bootloader, partition table, OTA data
and application:

```powershell
.\tools\flash_profile.ps1 `
  -Profile SNIFFER -Transport MQTT -Port <PORT> -Build
```

Replace `<PORT>` with the board's serial port. Do not use `-ForceActive` until
the final KAmod wiring has passed the gates in `HARDWARE.md`.

For the active image:

```powershell
.\tools\flash_profile.ps1 `
  -Profile MITM_NTS -Transport MQTT -Port <PORT> -Build -ForceActive
```

Disconnect USB before energizing the appliance unless isolation has been
independently verified.

## 5. Runtime checks

Open `http://klima-wifi.local/` or the assigned STA address. Check:

- `/api/status`: expected profile/version and translator state;
- `/api/ac-state`: fresh `main` and `panel` links;
- both frame counters increase;
- checksum and framing errors stay at zero;
- `control.available` is false in SNIFFER/BRIDGE and true only in verified
  `MITM_NTS` with both links ready.

Test a read before any write:

```powershell
py -3 tools\ac_control.py state
```

Then make one bounded command and wait for confirmation:

```powershell
py -3 tools\ac_control.py control --power on
py -3 tools\ac_control.py control --power off
```

The tool reads `.local/device.json` by default.

## 6. OTA

OTA updates only the application partition; it cannot migrate a partition
table. Build the same transport/layout, then run:

```powershell
py -3 tools\ota_update.py --device .local\device.json `
  --firmware build\release\mitm-nts-mqtt\klima_wifi.bin
```

The request uses `X-Klima-Token`. Firmware validates the image, boots the
alternate OTA slot and marks it valid only after core services start. Keep a
USB recovery path until the new version and both UART links are confirmed.

Changing between a legacy layout and the Zigbee layout requires a full USB
flash because `zb_storage` is part of the partition table.

## 7. Recovery

If the panel or bridge stops responding:

1. cut appliance power;
2. disconnect the bridge from the appliance;
3. reconnect the ESP by USB;
4. flash `SNIFFER` with `tools/flash_profile.ps1`;
5. inspect serial logs and wiring with the appliance unpowered;
6. do not return to `MITM_NTS` until both receive paths are proven again.

`tools/recover_usb.ps1` is a convenience wrapper for a USB recovery flash. It
never changes appliance wiring or bypasses the active-profile guard.

## 8. Local tests

```powershell
py -3 -m pytest -q tests
node --check zigbee2mqtt\klima_wifi_converter.js
```

On Linux/WSL:

```bash
cmake -S tests -B build/host-tests
cmake --build build/host-tests --parallel
ctest --test-dir build/host-tests --output-on-failure
```
