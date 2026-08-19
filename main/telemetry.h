#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint64_t sequence;
    char text[768];
} telemetry_entry_t;

esp_err_t telemetry_init(void);
void telemetry_publishf(const char *format, ...) __attribute__((format(printf, 1, 2)));
uint64_t telemetry_latest_sequence(void);
bool telemetry_get_after(uint64_t after, telemetry_entry_t *entry);
