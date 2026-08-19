#pragma once

// Copy this file to klima_secrets_local.h. The local file is ignored by Git.
#define KLIMA_WIFI_SSID "your-wifi"
#define KLIMA_WIFI_PASSWORD "your-password"
#define KLIMA_DEVICE_TOKEN "replace-with-a-long-random-token"
#define KLIMA_AP_PASSWORD "replace-with-a-different-random-password"

/* MQTT is deliberately disabled until a local secrets header supplies a
 * broker URI.  Keep credentials out of the repository. */
#define KLIMA_MQTT_BROKER_URI ""
#define KLIMA_MQTT_USERNAME ""
#define KLIMA_MQTT_PASSWORD ""
/* Empty selects the deterministic Wi-Fi-MAC-derived ID (klima_...). */
#define KLIMA_MQTT_DEVICE_ID ""
