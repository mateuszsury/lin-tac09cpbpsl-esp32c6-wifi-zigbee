# Security policy

## Supported version

Only the current `main` branch is supported while the project is pre-1.0.

## Reporting

Report vulnerabilities through the repository's private
[GitHub Security Advisory form](https://github.com/mateuszsury/esp32-c6-air-conditioner-mitm/security/advisories/new)
instead of a public issue. Do not include real Wi-Fi, MQTT, Home Assistant or
`X-Klima-Token` secrets in reports, captures or logs.

## Deployment assumptions

The firmware web API is designed for a trusted local network. HTTP and MQTT do
not provide end-to-end confidentiality by themselves; isolate the device on an
appropriate LAN/VLAN, use broker ACLs, keep the random device token private and
do not forward port 80 to the Internet.

`SNIFFER`/`PASSIVE` are recovery and diagnostic profiles. `MITM_NTS` may drive
appliance signal lines and must only be installed after all bypass jumpers are
removed, the physical endpoints are verified and a rollback path exists. Firmware
confirmation is not a replacement for electrical isolation or safe mains work.

An unconfigured build rejects every mutating HTTP endpoint. Use a unique device
token of at least 24 characters. The fallback AP starts only with an explicit
12-63 character password; otherwise it stays disabled.
