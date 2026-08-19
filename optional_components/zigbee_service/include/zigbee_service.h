#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the native ESP32-C6 Zigbee thermostat endpoint. */
esp_err_t zigbee_service_start(void);

/** Publish the current AC bridge state to the Zigbee data model. */
void zigbee_service_publish_state(void);

/** True after the endpoint has joined a Zigbee network. */
bool zigbee_service_is_joined(void);

#ifdef __cplusplus
}
#endif
