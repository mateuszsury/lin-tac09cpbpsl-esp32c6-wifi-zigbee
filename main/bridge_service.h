#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ac_protocol.h"
#include "esp_err.h"

typedef enum {
    BRIDGE_COMMAND_NONE = 0,
    BRIDGE_COMMAND_PENDING,
    BRIDGE_COMMAND_CONFIRMED,
    BRIDGE_COMMAND_TIMED_OUT,
    BRIDGE_COMMAND_CANCELLED,
} bridge_command_status_t;

typedef struct {
    bool main_valid;
    bool panel_valid;
    int64_t main_age_ms;
    int64_t panel_age_ms;
    bool power;
    uint8_t feature_flags;
    uint8_t setpoint_c;
    uint8_t main_display_temperature_c;
    uint8_t main_display_temperature_encoded;
    uint8_t mode_fan_code;
    ac_mode_t mode;
    ac_fan_t fan;
    uint8_t operation_code;
    uint8_t sensor_1_raw;
    uint8_t sensor_2_raw;
    bool panel_event;
    bool panel_emulated;
    bool mitm_active;
    bool override_active;
    bool command_pending;
    uint32_t command_sequence;
    bridge_command_status_t command_status;
    uint32_t command_timeouts;
    uint32_t main_frames;
    uint32_t panel_frames;
    uint32_t main_rx_bytes;
    uint32_t panel_rx_bytes;
    int8_t main_rx_level;
    int8_t panel_rx_level;
    int8_t alternate_panel_rx_level;
    int8_t panel_candidate_gpio5_level;
    int8_t panel_tx_pad_level;
    int8_t main_tx_pad_level;
    uint32_t mirror_main_edges;
    uint32_t alternate_panel_edges;
    uint32_t panel_candidate_gpio5_edges;
    uint32_t panel_tx_pad_edges;
    uint32_t main_tx_pad_edges;
    int8_t a4_monitor_level;
    int8_t b4_monitor_level;
    uint32_t a4_monitor_edges;
    uint32_t b4_monitor_edges;
    uint32_t b4_monitor_bytes;
    uint32_t b4_monitor_frames;
    uint32_t b4_monitor_checksum_errors;
    uint32_t b4_low_last_us;
    uint32_t b4_low_min_us;
    uint32_t b4_low_max_us;
    uint32_t b4_capture_lateness_us;
    uint32_t checksum_errors;
    uint32_t framing_errors;
    uint32_t injected_frames;
    uint8_t main_raw[AC_MAIN_FRAME_SIZE];
    uint8_t panel_raw[AC_PANEL_FRAME_SIZE];
    uint8_t forwarded_panel_raw[AC_PANEL_FRAME_SIZE];
    uint8_t b4_monitor_raw[AC_PANEL_FRAME_SIZE];
} bridge_ac_state_t;

esp_err_t bridge_service_start(void);
/* Preflight interlock for the KAmodNTS0104PW path.  Canonical NTS images
 * start fail-closed and may enable Wi-Fi control only after OE/rail/self-test
 * validation has completed.  Legacy profiles treat the interlock as already
 * ready for backwards compatibility. */
void bridge_service_set_hardware_ready(bool ready);
void bridge_service_get_ac_state(bridge_ac_state_t *state);
esp_err_t bridge_service_queue_control(
    const ac_control_request_t *request,
    uint32_t *sequence);
bool bridge_service_mitm_active(void);
const char *bridge_service_profile_name(void);
const char *bridge_service_command_status_name(bridge_command_status_t status);
