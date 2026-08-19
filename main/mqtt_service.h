#pragma once

#include "esp_err.h"

/* Starts the optional MQTT/HA transport.  An empty broker URI is a valid
 * disabled configuration and returns ESP_OK without creating a client. */
esp_err_t mqtt_service_start(void);

/* Safe to call from a status/diagnostic path; the worker also publishes on a
 * fixed interval.  The function is a no-op while MQTT is disabled/offline. */
void mqtt_service_publish_state(void);

