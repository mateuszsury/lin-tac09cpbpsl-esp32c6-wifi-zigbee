#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define CAPTURE_STORE_DATA_MAX 104U

typedef enum {
    CAPTURE_RECORD_FRAME = 1,
    CAPTURE_RECORD_MARKER = 2,
    CAPTURE_RECORD_BOOT = 3,
    CAPTURE_RECORD_SYSTEM = 4,
} capture_record_type_t;

typedef enum {
    CAPTURE_DIRECTION_NONE = 0,
    CAPTURE_DIRECTION_PANEL_TO_MAIN = 1,
    CAPTURE_DIRECTION_MAIN_TO_PANEL = 2,
} capture_direction_t;

typedef enum {
    CAPTURE_KIND_NONE = 0,
    CAPTURE_KIND_INITIAL = 1,
    CAPTURE_KIND_STATE_CHANGE = 2,
    CAPTURE_KIND_EVENT = 3,
    CAPTURE_KIND_WIFI_COMMAND = 4,
    CAPTURE_KIND_ERROR = 5,
} capture_kind_t;

typedef struct {
    uint32_t sequence;
    int64_t timestamp_us;
    capture_record_type_t type;
    capture_direction_t direction;
    capture_kind_t kind;
    uint8_t length;
    uint8_t data[CAPTURE_STORE_DATA_MAX];
} capture_store_record_t;

typedef struct {
    bool available;
    uint32_t capacity_records;
    uint32_t valid_records;
    uint32_t oldest_sequence;
    uint32_t latest_sequence;
    uint32_t dropped_records;
    uint32_t write_errors;
} capture_store_info_t;

esp_err_t capture_store_init(void);
void capture_store_record_frame(
    capture_direction_t direction,
    capture_kind_t kind,
    const uint8_t *data,
    size_t length);
esp_err_t capture_store_record_marker(const char *label);
esp_err_t capture_store_record_system(const char *event);
void capture_store_get_info(capture_store_info_t *info);
bool capture_store_get_after(uint32_t after, capture_store_record_t *record);
