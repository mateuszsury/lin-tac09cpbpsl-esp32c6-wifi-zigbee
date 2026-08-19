#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AC_PROTOCOL_HEADER_0 0x54U
#define AC_PROTOCOL_HEADER_1 0x44U
#define AC_PROTOCOL_XOR_TARGET 0x10U
/*
 * The length byte is the body size.  Both directions then carry one trailing
 * XOR check byte: panel = 2-byte header + length + 11-byte body + checksum
 * (15 wire bytes), main = 2 + 1 + 18 + 1 (22 wire bytes).  This was verified
 * again on the detached physical panel through the NTS0104 on 2026-08-19.
 */
#define AC_PANEL_BODY_LENGTH 0x0BU
#define AC_MAIN_BODY_LENGTH 0x12U
#define AC_PANEL_FRAME_SIZE 15U
#define AC_MAIN_FRAME_SIZE 22U
#define AC_PROTOCOL_MAX_FRAME_SIZE 64U
#define AC_FEATURE_UNITS_FAHRENHEIT 0x08U
#define AC_FEATURE_QUIET 0x10U
#define AC_FEATURE_TIMER 0x20U

typedef enum {
    AC_MODE_UNKNOWN = 0,
    AC_MODE_FAN = 2,
    AC_MODE_COOL = 3,
    AC_MODE_DRY = 4,
} ac_mode_t;

typedef enum {
    AC_FAN_UNKNOWN = 0,
    AC_FAN_LOW = 1,
    AC_FAN_MODE_DEFAULT = 2,
    AC_FAN_HIGH = 3,
} ac_fan_t;

typedef struct {
    bool event;
    bool power;
    uint8_t feature_flags;
    uint8_t mode_fan_code;
    ac_mode_t mode;
    ac_fan_t fan;
    uint8_t setpoint_c;
    uint8_t setpoint_encoded;
    uint8_t flags_7;
    uint8_t flags_8;
    uint8_t flags_10;
    uint8_t flags_11;
    uint8_t flags_12;
    uint8_t flags_13;
} ac_panel_state_t;

typedef struct {
    bool event;
    bool power;
    uint8_t feature_flags;
    uint8_t mode_fan_code;
    ac_mode_t mode;
    ac_fan_t fan;
    uint8_t display_temperature_c;
    uint8_t operation_code;
    uint8_t sensor_1_raw;
    uint8_t sensor_2_raw;
    uint8_t display_temperature_encoded;
    uint8_t flags_7;
    uint8_t flags_9;
    uint8_t flags_10;
    uint8_t flags_13;
    uint8_t constant_15;
} ac_main_state_t;

typedef enum {
    AC_CONTROL_POWER = 1U << 0,
    AC_CONTROL_SETPOINT = 1U << 1,
    AC_CONTROL_MODE = 1U << 2,
    AC_CONTROL_FAN = 1U << 3,
    AC_CONTROL_RAW_MODE_FAN = 1U << 4,
    AC_CONTROL_QUIET = 1U << 5,
    AC_CONTROL_UNITS_FAHRENHEIT = 1U << 6,
    AC_CONTROL_TIMER = 1U << 7,
} ac_control_mask_t;

typedef struct {
    uint32_t mask;
    bool power;
    uint8_t setpoint_c;
    ac_mode_t mode;
    ac_fan_t fan;
    uint8_t raw_mode_fan_code;
    bool quiet;
    bool units_fahrenheit;
    bool timer;
} ac_control_request_t;

uint8_t ac_protocol_xor(const uint8_t *data, size_t length);
bool ac_protocol_checksum_valid(const uint8_t *data, size_t length);
size_t ac_protocol_expected_size(const uint8_t *data, size_t available);
void ac_protocol_finalize(uint8_t *data, size_t length);

/* Returns the main-side mode/fan enum corresponding to a panel-side enum.
 * The PSL firmware normalizes FAN 0x22 on the panel wire to 0x21 in its main
 * response; all other observed enums are unchanged. */
uint8_t ac_protocol_panel_to_main_mode_fan(uint8_t panel_mode_fan_code);
uint8_t ac_protocol_main_to_panel_mode_fan(uint8_t main_mode_fan_code);

bool ac_protocol_parse_panel(
    const uint8_t frame[AC_PANEL_FRAME_SIZE],
    ac_panel_state_t *state);
bool ac_protocol_parse_main(
    const uint8_t frame[AC_MAIN_FRAME_SIZE],
    ac_main_state_t *state);

bool ac_protocol_valid_setpoint(uint8_t setpoint_c);
uint8_t ac_protocol_encode_setpoint(uint8_t setpoint_c);
bool ac_protocol_apply_control(
    uint8_t frame[AC_PANEL_FRAME_SIZE],
    const ac_control_request_t *request,
    bool event_frame);
bool ac_protocol_apply_control_main(
    uint8_t frame[AC_MAIN_FRAME_SIZE],
    const ac_control_request_t *request,
    bool event_frame);

const char *ac_protocol_mode_name(ac_mode_t mode);
const char *ac_protocol_fan_name(ac_fan_t fan);
