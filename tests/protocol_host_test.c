#include "ac_protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Complete UART captures from the PSL panel.  Every panel transaction is
 * exactly 15 bytes: 54 44 0B + 11-byte body + XOR checksum. */
static const char *const PANEL_CORPUS[] = {
    "54440B113100120000400000000079",
    "54440B1131801200004000000000F9",
    "54440B1131801300004200000000FA",
    "54440B1122801200004000000000EA",
    "54440B1133801200004000000000FB",
    "54440B113100120000400000002059",
    "54440B11438012000040000000008B",
    "54440B1231801200004000000000FA",
    "54440B1222801200004000000000E9",
    "54440B1231801300004200000000F9",
    "54440B124380120000400000000088",
    "54440B1233801200004000000000F8",
    "54440B12310012000040000000205A",
    "54440B1231901200004000000000EA",
    "54440B1231A01200004000000000DA",
    "54440B12310012000040000000007A",
};

static const char *const MAIN_CORPUS[] = {
    "54441211318012000200002C2900400A0000000000ED",
    "54441211338012000200002C2900400A0000000000EF",
    "54441211218016000200002C2900480A0000000000F1",
    "54441211218000000200002C2900200A00000000008F",
    "54441211438016000200002C2900480A000000000093",
    "54441212318012000200002C2900400A0000000000EE",
    "54441212218000000200002C2900200A00000000008C",
    "54441212218016000200002C2900480A0000000000F2",
    "54441212438016000200002C2900480A000000000090",
    "54441212338012000200002C2900400A0000000000EC",
    "54441212319012000000002E2C00400A00800000007B",
    "5444121231A012000000002E2D00400A00800000004A",
    "54441212310012000000002E2D00400A0080000000EA",
};

static const uint8_t PANEL_OFF_18[AC_PANEL_FRAME_SIZE] = {
    0x54, 0x44, 0x0B, 0x11, 0x31, 0x00, 0x12, 0x00,
    0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x79,
};

static const uint8_t MAIN_OFF_18[AC_MAIN_FRAME_SIZE] = {
    0x54, 0x44, 0x12, 0x11, 0x31, 0x00, 0x12, 0x00,
    0x00, 0x00, 0x00, 0x2C, 0x2C, 0x00, 0x40, 0x0A,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x6A,
};

static uint8_t hex_nibble(char value)
{
    if (value >= '0' && value <= '9') {
        return (uint8_t)(value - '0');
    }
    if (value >= 'A' && value <= 'F') {
        return (uint8_t)(value - 'A' + 10);
    }
    assert(false);
    return 0U;
}

static void decode_hex(const char *hex, uint8_t *output, size_t length)
{
    assert(strlen(hex) == length * 2U);
    for (size_t i = 0; i < length; ++i) {
        output[i] = (uint8_t)((hex_nibble(hex[i * 2U]) << 4U) |
                              hex_nibble(hex[i * 2U + 1U]));
    }
}

static void test_complete_capture_corpus(void)
{
    for (size_t i = 0; i < sizeof(PANEL_CORPUS) / sizeof(PANEL_CORPUS[0]); ++i) {
        uint8_t frame[AC_PANEL_FRAME_SIZE];
        ac_panel_state_t state;
        decode_hex(PANEL_CORPUS[i], frame, sizeof(frame));
        assert(ac_protocol_expected_size(frame, 3U) == AC_PANEL_FRAME_SIZE);
        assert(ac_protocol_checksum_valid(frame, sizeof(frame)));
        assert(ac_protocol_parse_panel(frame, &state));
        assert(state.mode != AC_MODE_UNKNOWN);
        assert(state.setpoint_c == 18U || state.setpoint_c == 19U);
        assert(state.setpoint_encoded == ac_protocol_encode_setpoint(state.setpoint_c));
    }

    for (size_t i = 0; i < sizeof(MAIN_CORPUS) / sizeof(MAIN_CORPUS[0]); ++i) {
        uint8_t frame[AC_MAIN_FRAME_SIZE];
        ac_main_state_t state;
        decode_hex(MAIN_CORPUS[i], frame, sizeof(frame));
        assert(ac_protocol_checksum_valid(frame, sizeof(frame)));
        assert(ac_protocol_parse_main(frame, &state));
        assert(state.mode != AC_MODE_UNKNOWN);
        if (state.display_temperature_c == 0U) {
            assert(state.display_temperature_encoded == 32U);
        } else {
            assert(state.display_temperature_encoded ==
                   ac_protocol_encode_setpoint(state.display_temperature_c));
        }
    }
}

static void test_known_frames(void)
{
    assert(ac_protocol_expected_size(PANEL_OFF_18, 3U) == AC_PANEL_FRAME_SIZE);
    assert(ac_protocol_expected_size(MAIN_OFF_18, 3U) == AC_MAIN_FRAME_SIZE);
    assert(ac_protocol_checksum_valid(PANEL_OFF_18, sizeof(PANEL_OFF_18)));
    assert(ac_protocol_checksum_valid(MAIN_OFF_18, sizeof(MAIN_OFF_18)));

    ac_panel_state_t panel;
    assert(ac_protocol_parse_panel(PANEL_OFF_18, &panel));
    assert(!panel.power);
    assert(!panel.event);
    assert(panel.mode == AC_MODE_COOL);
    assert(panel.fan == AC_FAN_LOW);
    assert(panel.setpoint_c == 18U);
    assert(panel.setpoint_encoded == 0x40U);

    ac_main_state_t main;
    assert(ac_protocol_parse_main(MAIN_OFF_18, &main));
    assert(!main.power);
    assert(main.mode_fan_code == 0x31U);
    assert(main.sensor_1_raw == 0x2CU);
    assert(main.sensor_2_raw == 0x2CU);

    uint8_t high_fan[AC_PANEL_FRAME_SIZE];
    decode_hex("54440B1233801200004000000000F8", high_fan, sizeof(high_fan));
    assert(ac_protocol_parse_panel(high_fan, &panel));
    assert(panel.mode == AC_MODE_COOL);
    assert(panel.fan == AC_FAN_HIGH);
    assert(strcmp(ac_protocol_fan_name(panel.fan), "high") == 0);

    uint8_t fan_display[AC_MAIN_FRAME_SIZE];
    decode_hex(
        "54441212218018000200002E2D004B0A0000000000F9",
        fan_display,
        sizeof(fan_display));
    assert(ac_protocol_parse_main(fan_display, &main));
    assert(main.mode == AC_MODE_FAN);
    assert(main.display_temperature_c == 24U);
    assert(main.display_temperature_encoded == 75U);

    uint8_t feature_frame[AC_PANEL_FRAME_SIZE];
    decode_hex("54440B1231A01200004000000000DA", feature_frame, sizeof(feature_frame));
    assert(ac_protocol_parse_panel(feature_frame, &panel));
    assert(panel.power);
    assert(panel.feature_flags == 0x20U);
}

static void test_checksum_generation(void)
{
    uint8_t panel[AC_PANEL_FRAME_SIZE];
    memcpy(panel, PANEL_OFF_18, sizeof(panel));
    panel[AC_PANEL_FRAME_SIZE - 1U] = 0U;
    ac_protocol_finalize(panel, sizeof(panel));
    assert(panel[AC_PANEL_FRAME_SIZE - 1U] == 0x79U);
    assert(ac_protocol_xor(panel, sizeof(panel)) == AC_PROTOCOL_XOR_TARGET);
    assert(ac_protocol_checksum_valid(panel, sizeof(panel)));

    uint8_t main[AC_MAIN_FRAME_SIZE];
    memcpy(main, MAIN_OFF_18, sizeof(main));
    main[AC_MAIN_FRAME_SIZE - 1U] = 0U;
    ac_protocol_finalize(main, sizeof(main));
    assert(main[AC_MAIN_FRAME_SIZE - 1U] == 0x6AU);
    assert(ac_protocol_xor(main, sizeof(main)) == AC_PROTOCOL_XOR_TARGET);
}

static void test_temperature_encoding(void)
{
    static const uint8_t expected_fahrenheit[] = {
        64U, 66U, 68U, 70U, 72U, 73U, 75U, 77U,
        79U, 81U, 82U, 84U, 86U, 88U, 90U,
    };
    for (uint8_t temperature = 18U; temperature <= 32U; ++temperature) {
        assert(ac_protocol_valid_setpoint(temperature));
        assert(ac_protocol_encode_setpoint(temperature) ==
               expected_fahrenheit[temperature - 18U]);
    }
    assert(!ac_protocol_valid_setpoint(17U));
    assert(!ac_protocol_valid_setpoint(33U));
}

static void test_control_mutation(void)
{
    uint8_t frame[AC_PANEL_FRAME_SIZE];
    memcpy(frame, PANEL_OFF_18, sizeof(frame));
    const ac_control_request_t power_and_temperature = {
        .mask = AC_CONTROL_POWER | AC_CONTROL_SETPOINT,
        .power = true,
        .setpoint_c = 32U,
    };
    assert(ac_protocol_apply_control(frame, &power_and_temperature, true));
    assert(frame[3] == 0x12U);
    assert((frame[5] & 0x80U) != 0U);
    assert(frame[6] == 32U);
    assert(frame[9] == 0x5AU);
    assert(ac_protocol_checksum_valid(frame, sizeof(frame)));

    const ac_control_request_t dry = {
        .mask = AC_CONTROL_MODE,
        .mode = AC_MODE_DRY,
    };
    assert(ac_protocol_apply_control(frame, &dry, false));
    assert(frame[3] == 0x11U);
    assert(frame[4] == 0x43U);

    const ac_control_request_t fan_mode = {
        .mask = AC_CONTROL_MODE,
        .mode = AC_MODE_FAN,
    };
    assert(ac_protocol_apply_control(frame, &fan_mode, true));
    assert(frame[4] == 0x22U);

    const ac_control_request_t cool_high = {
        .mask = AC_CONTROL_MODE | AC_CONTROL_FAN,
        .mode = AC_MODE_COOL,
        .fan = AC_FAN_HIGH,
    };
    assert(ac_protocol_apply_control(frame, &cool_high, true));
    assert(frame[4] == 0x33U);
    assert(ac_protocol_checksum_valid(frame, sizeof(frame)));

    const ac_control_request_t power_off = {
        .mask = AC_CONTROL_POWER,
        .power = false,
    };
    assert(ac_protocol_apply_control(frame, &power_off, true));
    assert((frame[5] & 0x80U) == 0U);
    /* Every control mutation must regenerate the panel checksum. */
    assert(ac_protocol_checksum_valid(frame, sizeof(frame)));

    const ac_control_request_t features = {
        .mask = AC_CONTROL_QUIET | AC_CONTROL_UNITS_FAHRENHEIT | AC_CONTROL_TIMER,
        .quiet = true,
        .units_fahrenheit = true,
        .timer = true,
    };
    assert(ac_protocol_apply_control(frame, &features, true));
    assert((frame[5] & AC_FEATURE_QUIET) != 0U);
    assert((frame[5] & AC_FEATURE_UNITS_FAHRENHEIT) != 0U);
    assert((frame[5] & AC_FEATURE_TIMER) != 0U);

    uint8_t cold_off[AC_PANEL_FRAME_SIZE];
    decode_hex(
        "54440B11410000000000000000005B",
        cold_off,
        sizeof(cold_off));
    const ac_control_request_t cold_power_on = {
        .mask = AC_CONTROL_POWER,
        .power = true,
    };
    assert(ac_protocol_apply_control(cold_off, &cold_power_on, true));
    uint8_t captured_power_on[AC_PANEL_FRAME_SIZE];
    decode_hex(
        "54440B1231801200004000000000FA",
        captured_power_on,
        sizeof(captured_power_on));
    assert(memcmp(cold_off, captured_power_on, sizeof(cold_off)) == 0);
}

static void test_invalid_input_and_enum_translation(void)
{
    uint8_t panel[AC_PANEL_FRAME_SIZE];
    memcpy(panel, PANEL_OFF_18, sizeof(panel));
    panel[2] = AC_MAIN_BODY_LENGTH;
    ac_panel_state_t parsed;
    assert(!ac_protocol_parse_panel(panel, &parsed));

    uint8_t main[AC_MAIN_FRAME_SIZE];
    memcpy(main, MAIN_OFF_18, sizeof(main));
    main[6] ^= 1U;
    ac_main_state_t main_state;
    assert(!ac_protocol_parse_main(main, &main_state));

    const ac_control_request_t invalid_temperature = {
        .mask = AC_CONTROL_SETPOINT,
        .setpoint_c = 17U,
    };
    memcpy(panel, PANEL_OFF_18, sizeof(panel));
    assert(!ac_protocol_apply_control(panel, &invalid_temperature, true));
    assert(ac_protocol_panel_to_main_mode_fan(0x22U) == 0x21U);
    assert(ac_protocol_main_to_panel_mode_fan(0x21U) == 0x22U);
    assert(ac_protocol_panel_to_main_mode_fan(0x33U) == 0x33U);
}

int main(void)
{
    test_complete_capture_corpus();
    test_known_frames();
    test_checksum_generation();
    test_temperature_encoding();
    test_control_mutation();
    test_invalid_input_and_enum_translation();
    puts("protocol_host_test: PASS");
    return 0;
}
