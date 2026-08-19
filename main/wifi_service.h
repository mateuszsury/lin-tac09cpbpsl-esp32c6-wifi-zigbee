#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    bool sta_connected;
    bool ap_enabled;
    char sta_ip[16];
    char ap_ssid[33];
    char ap_password[65];
} wifi_service_status_t;

esp_err_t wifi_service_start(void);
void wifi_service_get_status(wifi_service_status_t *status);
