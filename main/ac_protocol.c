#include "ac_protocol.h"

#include <string.h>

uint8_t ac_protocol_xor(const uint8_t *data, size_t length)
{
    uint8_t value = 0U;
    if (!data) {
        return value;
    }
    for (size_t i = 0; i < length; ++i) {
        value ^= data[i];
    }
    return value;
}

bool ac_protocol_checksum_valid(const uint8_t *data, size_t length)
{
    if (!data || length < 3U) {
        return false;
    }

    const bool known_size =
        (data[2] == AC_PANEL_BODY_LENGTH && length == AC_PANEL_FRAME_SIZE) ||
        (data[2] == AC_MAIN_BODY_LENGTH && length == AC_MAIN_FRAME_SIZE);
    return known_size && ac_protocol_xor(data, length) == AC_PROTOCOL_XOR_TARGET;
}

size_t ac_protocol_expected_size(const uint8_t *data, size_t available)
{
    if (!data || available < 3U || data[0] != AC_PROTOCOL_HEADER_0 ||
        data[1] != AC_PROTOCOL_HEADER_1) {
        return 0U;
    }
    if (data[2] == AC_PANEL_BODY_LENGTH) {
        return AC_PANEL_FRAME_SIZE;
    }
    if (data[2] == AC_MAIN_BODY_LENGTH) {
        return AC_MAIN_FRAME_SIZE;
    }
    /* Unknown length values are not valid frames for this appliance. */
    return 0U;
}

void ac_protocol_finalize(uint8_t *data, size_t length)
{
    if (!data || length < 3U) {
        return;
    }
    if ((data[2] == AC_PANEL_BODY_LENGTH && length == AC_PANEL_FRAME_SIZE) ||
        (data[2] == AC_MAIN_BODY_LENGTH && length == AC_MAIN_FRAME_SIZE)) {
        data[length - 1U] = ac_protocol_xor(data, length - 1U) ^
            AC_PROTOCOL_XOR_TARGET;
    }
}

uint8_t ac_protocol_panel_to_main_mode_fan(uint8_t panel_mode_fan_code)
{
    return panel_mode_fan_code == 0x22U ? 0x21U : panel_mode_fan_code;
}

uint8_t ac_protocol_main_to_panel_mode_fan(uint8_t main_mode_fan_code)
{
    return main_mode_fan_code == 0x21U ? 0x22U : main_mode_fan_code;
}

static ac_mode_t decode_mode(uint8_t mode_fan_code)
{
    const uint8_t code = (mode_fan_code >> 4U) & 0x0FU;
    return code == AC_MODE_FAN || code == AC_MODE_COOL || code == AC_MODE_DRY
        ? (ac_mode_t)code
        : AC_MODE_UNKNOWN;
}

static ac_fan_t decode_fan(uint8_t mode_fan_code)
{
    const uint8_t code = mode_fan_code & 0x0FU;
    return code >= AC_FAN_LOW && code <= AC_FAN_HIGH
        ? (ac_fan_t)code
        : AC_FAN_UNKNOWN;
}

bool ac_protocol_parse_panel(
    const uint8_t frame[AC_PANEL_FRAME_SIZE],
    ac_panel_state_t *state)
{
    if (!frame || !state || ac_protocol_expected_size(frame, 3U) != AC_PANEL_FRAME_SIZE ||
        !ac_protocol_checksum_valid(frame, AC_PANEL_FRAME_SIZE)) {
        return false;
    }

    *state = (ac_panel_state_t){
        .event = frame[3] == 0x12U,
        .power = (frame[5] & 0x80U) != 0U,
        .feature_flags = frame[5] & 0x7FU,
        .mode_fan_code = frame[4],
        .mode = decode_mode(frame[4]),
        .fan = decode_fan(frame[4]),
        .setpoint_c = frame[6],
        .setpoint_encoded = frame[9],
        .flags_7 = frame[7],
        .flags_8 = frame[8],
        .flags_10 = frame[10],
        .flags_11 = frame[11],
        .flags_12 = frame[12],
        .flags_13 = frame[13],
    };
    return true;
}

bool ac_protocol_parse_main(
    const uint8_t frame[AC_MAIN_FRAME_SIZE],
    ac_main_state_t *state)
{
    if (!frame || !state || ac_protocol_expected_size(frame, 3U) != AC_MAIN_FRAME_SIZE ||
        !ac_protocol_checksum_valid(frame, AC_MAIN_FRAME_SIZE)) {
        return false;
    }

    *state = (ac_main_state_t){
        .event = frame[3] == 0x12U,
        .power = (frame[5] & 0x80U) != 0U,
        .feature_flags = frame[5] & 0x7FU,
        .mode_fan_code = frame[4],
        .mode = decode_mode(frame[4]),
        .fan = decode_fan(frame[4]),
        .display_temperature_c = frame[6],
        .operation_code = frame[8],
        .sensor_1_raw = frame[11],
        .sensor_2_raw = frame[12],
        .display_temperature_encoded = frame[14],
        .flags_7 = frame[7],
        .flags_9 = frame[9],
        .flags_10 = frame[10],
        .flags_13 = frame[13],
        .constant_15 = frame[15],
    };
    return true;
}

bool ac_protocol_valid_setpoint(uint8_t setpoint_c)
{
    return setpoint_c >= 18U && setpoint_c <= 32U;
}

uint8_t ac_protocol_encode_setpoint(uint8_t setpoint_c)
{
    /* The second temperature field is Fahrenheit rounded to nearest integer:
     * 18C -> 64F, 19C -> 66F, 22C -> 72F. */
    return (uint8_t)((((unsigned)setpoint_c * 9U) + 2U) / 5U + 32U);
}

static uint8_t requested_mode_fan(const ac_panel_state_t *current, const ac_control_request_t *request)
{
    if ((request->mask & AC_CONTROL_RAW_MODE_FAN) != 0U) {
        return request->raw_mode_fan_code;
    }

    ac_mode_t mode = (request->mask & AC_CONTROL_MODE) != 0U
        ? request->mode
        : current->mode;
    ac_fan_t fan = (request->mask & AC_CONTROL_FAN) != 0U
        ? request->fan
        : current->fan;

    if (mode == AC_MODE_DRY) {
        return 0x43U; /* Only dry code observed on the exact hardware. */
    }
    if (mode == AC_MODE_FAN) {
        /* 0x22 is the exact panel-side FAN state captured on this unit. */
        return 0x22U;
    }
    if (mode == AC_MODE_COOL) {
        if (fan != AC_FAN_LOW && fan != AC_FAN_HIGH) {
            fan = AC_FAN_LOW;
        }
        return (uint8_t)(0x30U | (uint8_t)fan);
    }
    return current->mode_fan_code;
}

bool ac_protocol_apply_control(
    uint8_t frame[AC_PANEL_FRAME_SIZE],
    const ac_control_request_t *request,
    bool event_frame)
{
    ac_panel_state_t current;
    if (!frame || !request || !ac_protocol_parse_panel(frame, &current)) {
        return false;
    }
    if ((request->mask & AC_CONTROL_SETPOINT) != 0U &&
        !ac_protocol_valid_setpoint(request->setpoint_c)) {
        return false;
    }
    if ((request->mask & AC_CONTROL_MODE) != 0U &&
        request->mode != AC_MODE_COOL && request->mode != AC_MODE_FAN &&
        request->mode != AC_MODE_DRY) {
        return false;
    }
    if ((request->mask & AC_CONTROL_FAN) != 0U &&
        request->fan != AC_FAN_LOW && request->fan != AC_FAN_HIGH) {
        return false;
    }

    frame[3] = event_frame ? 0x12U : 0x11U;
    /* A cold controller reports OFF as mode 0x41 with a zero temperature.
     * Merely setting the power bit would produce an invalid 0 C command that
     * the main PCB ignores. The exact physical POWER ON capture starts in
     * Cool/Low at 18 C (0x31, 0x12, 0x40), so bootstrap that state whenever
     * no valid remembered setpoint exists. Explicit mode/fan/setpoint fields
     * below still take precedence. */
    if ((request->mask & AC_CONTROL_POWER) != 0U && request->power &&
        !ac_protocol_valid_setpoint(current.setpoint_c)) {
        frame[4] = 0x31U;
        frame[6] = 18U;
        frame[9] = ac_protocol_encode_setpoint(18U);
        current.mode_fan_code = 0x31U;
        current.mode = AC_MODE_COOL;
        current.fan = AC_FAN_LOW;
        current.setpoint_c = 18U;
        current.setpoint_encoded = frame[9];
    }
    if ((request->mask & (AC_CONTROL_MODE | AC_CONTROL_FAN | AC_CONTROL_RAW_MODE_FAN)) != 0U) {
        frame[4] = requested_mode_fan(&current, request);
    }
    if ((request->mask & AC_CONTROL_POWER) != 0U) {
        frame[5] = request->power ? (uint8_t)(frame[5] | 0x80U)
                                  : (uint8_t)(frame[5] & (uint8_t)~0x80U);
    }
    if ((request->mask & AC_CONTROL_SETPOINT) != 0U) {
        frame[6] = request->setpoint_c;
        frame[9] = ac_protocol_encode_setpoint(request->setpoint_c);
    }
    if ((request->mask & AC_CONTROL_QUIET) != 0U) {
        frame[5] = request->quiet
            ? (uint8_t)(frame[5] | AC_FEATURE_QUIET)
            : (uint8_t)(frame[5] & (uint8_t)~AC_FEATURE_QUIET);
    }
    if ((request->mask & AC_CONTROL_UNITS_FAHRENHEIT) != 0U) {
        frame[5] = request->units_fahrenheit
            ? (uint8_t)(frame[5] | AC_FEATURE_UNITS_FAHRENHEIT)
            : (uint8_t)(frame[5] & (uint8_t)~AC_FEATURE_UNITS_FAHRENHEIT);
    }
    if ((request->mask & AC_CONTROL_TIMER) != 0U) {
        frame[5] = request->timer
            ? (uint8_t)(frame[5] | AC_FEATURE_TIMER)
            : (uint8_t)(frame[5] & (uint8_t)~AC_FEATURE_TIMER);
    }
    ac_protocol_finalize(frame, AC_PANEL_FRAME_SIZE);
    return true;
}

bool ac_protocol_apply_control_main(
    uint8_t frame[AC_MAIN_FRAME_SIZE],
    const ac_control_request_t *request,
    bool event_frame)
{
    ac_main_state_t current_main;
    if (!frame || !request || !ac_protocol_parse_main(frame, &current_main)) {
        return false;
    }
    if ((request->mask & AC_CONTROL_SETPOINT) != 0U &&
        !ac_protocol_valid_setpoint(request->setpoint_c)) {
        return false;
    }
    if ((request->mask & AC_CONTROL_MODE) != 0U &&
        request->mode != AC_MODE_COOL && request->mode != AC_MODE_FAN &&
        request->mode != AC_MODE_DRY) {
        return false;
    }
    if ((request->mask & AC_CONTROL_FAN) != 0U &&
        request->fan != AC_FAN_LOW && request->fan != AC_FAN_HIGH) {
        return false;
    }

    ac_panel_state_t current_panel = {
        .mode_fan_code = ac_protocol_main_to_panel_mode_fan(current_main.mode_fan_code),
        .mode = current_main.mode,
        .fan = current_main.fan,
    };
    frame[3] = event_frame ? 0x12U : 0x11U;
    if ((request->mask & AC_CONTROL_POWER) != 0U && request->power &&
        !ac_protocol_valid_setpoint(current_main.display_temperature_c)) {
        frame[4] = 0x31U;
        frame[6] = 18U;
        frame[14] = ac_protocol_encode_setpoint(18U);
        current_panel.mode_fan_code = 0x31U;
        current_panel.mode = AC_MODE_COOL;
        current_panel.fan = AC_FAN_LOW;
    }
    if ((request->mask &
         (AC_CONTROL_MODE | AC_CONTROL_FAN | AC_CONTROL_RAW_MODE_FAN)) != 0U) {
        frame[4] = ac_protocol_panel_to_main_mode_fan(
            requested_mode_fan(&current_panel, request));
    }
    if ((request->mask & AC_CONTROL_POWER) != 0U) {
        frame[5] = request->power ? (uint8_t)(frame[5] | 0x80U)
                                  : (uint8_t)(frame[5] & (uint8_t)~0x80U);
    }
    if ((request->mask & AC_CONTROL_SETPOINT) != 0U) {
        frame[6] = request->setpoint_c;
        frame[14] = ac_protocol_encode_setpoint(request->setpoint_c);
    }
    if ((request->mask & AC_CONTROL_QUIET) != 0U) {
        frame[5] = request->quiet
            ? (uint8_t)(frame[5] | AC_FEATURE_QUIET)
            : (uint8_t)(frame[5] & (uint8_t)~AC_FEATURE_QUIET);
    }
    if ((request->mask & AC_CONTROL_UNITS_FAHRENHEIT) != 0U) {
        frame[5] = request->units_fahrenheit
            ? (uint8_t)(frame[5] | AC_FEATURE_UNITS_FAHRENHEIT)
            : (uint8_t)(frame[5] & (uint8_t)~AC_FEATURE_UNITS_FAHRENHEIT);
    }
    if ((request->mask & AC_CONTROL_TIMER) != 0U) {
        frame[5] = request->timer
            ? (uint8_t)(frame[5] | AC_FEATURE_TIMER)
            : (uint8_t)(frame[5] & (uint8_t)~AC_FEATURE_TIMER);
    }
    ac_protocol_finalize(frame, AC_MAIN_FRAME_SIZE);
    return true;
}

const char *ac_protocol_mode_name(ac_mode_t mode)
{
    switch (mode) {
    case AC_MODE_FAN:
        return "fan";
    case AC_MODE_COOL:
        return "cool";
    case AC_MODE_DRY:
        return "dry";
    default:
        return "unknown";
    }
}

const char *ac_protocol_fan_name(ac_fan_t fan)
{
    switch (fan) {
    case AC_FAN_LOW:
        return "low";
    case AC_FAN_MODE_DEFAULT:
        return "mode-default";
    case AC_FAN_HIGH:
        return "high";
    default:
        return "unknown";
    }
}
