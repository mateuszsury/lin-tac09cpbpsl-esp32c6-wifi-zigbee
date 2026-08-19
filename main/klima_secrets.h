#pragma once

#if __has_include("klima_secrets_local.h")
#include "klima_secrets_local.h"
#else
#define KLIMA_WIFI_SSID ""
#define KLIMA_WIFI_PASSWORD ""
#define KLIMA_DEVICE_TOKEN ""
#endif

#ifndef KLIMA_AP_PASSWORD
#define KLIMA_AP_PASSWORD KLIMA_DEVICE_TOKEN
#endif

/* Safe empty defaults keep a build usable without a broker or credentials.
 * Local deployments may define these in klima_secrets_local.h or with
 * compiler definitions; no secret values belong in this tracked header. */
#ifndef KLIMA_MQTT_BROKER_URI
#ifdef KLIMA_MQTT_URI
#define KLIMA_MQTT_BROKER_URI KLIMA_MQTT_URI
#else
#define KLIMA_MQTT_BROKER_URI ""
#endif
#endif

#ifndef KLIMA_MQTT_USERNAME
#ifdef KLIMA_MQTT_USER
#define KLIMA_MQTT_USERNAME KLIMA_MQTT_USER
#else
#define KLIMA_MQTT_USERNAME ""
#endif
#endif

#ifndef KLIMA_MQTT_PASSWORD
#ifdef KLIMA_MQTT_PASS
#define KLIMA_MQTT_PASSWORD KLIMA_MQTT_PASS
#else
#define KLIMA_MQTT_PASSWORD ""
#endif
#endif

#ifndef KLIMA_MQTT_DEVICE_ID
#define KLIMA_MQTT_DEVICE_ID ""
#endif
