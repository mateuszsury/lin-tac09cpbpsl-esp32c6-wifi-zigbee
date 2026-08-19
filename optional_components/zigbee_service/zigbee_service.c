#include "zigbee_service.h"

#include <inttypes.h>
#include <string.h>

#include "bridge_service.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_zigbee.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ezbee/bdb.h"
#include "ezbee/core.h"
#include "ezbee/nwk.h"
#include "ezbee/zcl/cluster/basic_desc.h"
#include "ezbee/zcl/cluster/custom.h"
#include "ezbee/zcl/cluster/fan_control_desc.h"
#include "ezbee/zcl/cluster/thermostat_desc.h"
#include "ezbee/zcl/zcl_common.h"
#include "ezbee/zcl/zcl_core.h"
#include "ezbee/zcl/zcl_reporting.h"
#include "ezbee/zha.h"

static const char *TAG = "zigbee_service";
/* Stable public identity used by Zigbee2MQTT discovery. */

#define ZIGBEE_ENDPOINT 1U
#define ZIGBEE_PRIMARY_CHANNEL_MASK 0x07FFF800U
#define KLIMA_CLUSTER_ID 0xFC10U
#define KLIMA_MANUFACTURER_CODE 0x131BU

/* Manufacturer-specific diagnostics are read-only.  Three action-labelled
 * feature flags use read/write/reporting attributes and, like standard HVAC
 * writes, are queued through the AC bridge confirmation path. */
#define KLIMA_ATTR_CONTROL_AVAILABLE 0x0001U
#define KLIMA_ATTR_RAW_MODE_FAN_CODE 0x0002U
#define KLIMA_ATTR_OPERATION_CODE 0x0003U
#define KLIMA_ATTR_MAIN_LINK_VALID 0x0004U
#define KLIMA_ATTR_PANEL_LINK_VALID 0x0005U
#define KLIMA_ATTR_MAIN_AGE_MS 0x0006U
#define KLIMA_ATTR_PANEL_AGE_MS 0x0007U
#define KLIMA_ATTR_SENSOR_1_RAW 0x0008U
#define KLIMA_ATTR_SENSOR_2_RAW 0x0009U
#define KLIMA_ATTR_MITM_ACTIVE 0x000AU
#define KLIMA_ATTR_COMMAND_PENDING 0x000BU
#define KLIMA_ATTR_COMMAND_SEQUENCE 0x000CU
#define KLIMA_ATTR_CHECKSUM_ERRORS 0x000DU
#define KLIMA_ATTR_FRAMING_ERRORS 0x000EU
#define KLIMA_ATTR_INJECTED_FRAMES 0x000FU
#define KLIMA_ATTR_PANEL_EVENT 0x0010U
#define KLIMA_ATTR_COMMAND_STATUS 0x0011U
#define KLIMA_ATTR_COMMAND_TIMEOUTS 0x0012U
#define KLIMA_ATTR_QUIET 0x0020U
#define KLIMA_ATTR_UNITS_FAHRENHEIT 0x0021U
#define KLIMA_ATTR_TIMER 0x0022U
/* Stable names used by host tooling and external converters. */
#define KLIMA_ZCL_CLUSTER_ID KLIMA_CLUSTER_ID
#define KLIMA_ZCL_MANUFACTURER_CODE KLIMA_MANUFACTURER_CODE
#define KLIMA_ZCL_ATTR_CONTROL_AVAILABLE_ID KLIMA_ATTR_CONTROL_AVAILABLE
#define KLIMA_ZCL_ATTR_RAW_MODE_FAN_CODE_ID KLIMA_ATTR_RAW_MODE_FAN_CODE
#define KLIMA_ZCL_ATTR_OPERATION_CODE_ID KLIMA_ATTR_OPERATION_CODE

/* ZCL string: "Klima WiFi contributors". */
static const uint8_t s_manufacturer_name[] = {
    23, 'K', 'l', 'i', 'm', 'a', ' ', 'W', 'i', 'F', 'i', ' ',
    'c', 'o', 'n', 't', 'r', 'i', 'b', 'u', 't', 'o', 'r', 's'
};
/* ZCL string: "Klima WiFi AC Bridge". */
static const uint8_t s_model_identifier[] = {
    20, 'K', 'l', 'i', 'm', 'a', ' ', 'W', 'i', 'F', 'i', ' ',
    'A', 'C', ' ', 'B', 'r', 'i', 'd', 'g', 'e'
};
/* ZCL character string: byte 0 is the length. Populated from the same app
 * descriptor that drives HTTP/MQTT version reporting, so Zigbee interviews
 * cannot retain a stale handwritten firmware version. */
static uint8_t s_sw_build_id[33];

typedef struct {
    bool started;
    volatile bool joined;
    bool control_available;
    uint8_t system_mode;
    uint8_t fan_mode;
    int16_t local_temperature;
    int16_t occupied_cooling_setpoint;
    int16_t unoccupied_cooling_setpoint;
    uint8_t running_mode;
    uint8_t fan_mode_sequence;
    bool main_link_valid;
    bool panel_link_valid;
    bool mitm_active;
    bool command_pending;
    bool panel_event;
    bool quiet;
    bool units_fahrenheit;
    bool timer;
    bool rf_connected;
    uint8_t raw_mode_fan_code;
    uint8_t operation_code;
    uint8_t sensor_1_raw;
    uint8_t sensor_2_raw;
    uint8_t command_status;
    int32_t main_age_ms;
    int32_t panel_age_ms;
    uint32_t command_sequence;
    uint32_t command_timeouts;
    uint32_t checksum_errors;
    uint32_t framing_errors;
    uint32_t injected_frames;
    uint32_t report_failures;
} zigbee_ctx_t;

static zigbee_ctx_t s_zigbee;

static void report_attr(uint8_t endpoint, uint16_t cluster_id, uint16_t attr_id, uint16_t manuf_code)
{
    ezb_zcl_reporting_info_t info = ezb_zcl_reporting_info_find(
        endpoint, cluster_id, EZB_ZCL_CLUSTER_SERVER, attr_id, manuf_code);
    if (info == EZB_ZCL_INVALID_REPORTING_INFO) {
        s_zigbee.report_failures++;
        return;
    }
    if (ezb_zcl_reporting_start_attr_report(info) != EZB_ERR_NONE) {
        s_zigbee.report_failures++;
    }
}

static void set_attr_report(uint8_t endpoint, uint16_t cluster_id, uint16_t attr_id,
                            uint16_t manuf_code, void *value)
{
    ezb_zcl_status_t status = ezb_zcl_set_attr_value(
        endpoint, cluster_id, EZB_ZCL_CLUSTER_SERVER, attr_id, manuf_code, value, false);
    if (status == EZB_ZCL_STATUS_SUCCESS) {
        report_attr(endpoint, cluster_id, attr_id, manuf_code);
    } else {
        ESP_LOGD(TAG, "attribute update failed cluster=0x%04x attr=0x%04x status=0x%02x",
                 cluster_id, attr_id, status);
    }
}

static uint8_t system_mode_for_state(const bridge_ac_state_t *state)
{
    if (!state->power) {
        return EZB_ZCL_THERMOSTAT_SYSTEM_MODE_OFF;
    }
    switch (state->mode) {
    case AC_MODE_COOL: return EZB_ZCL_THERMOSTAT_SYSTEM_MODE_COOL;
    case AC_MODE_FAN:  return EZB_ZCL_THERMOSTAT_SYSTEM_MODE_FAN_ONLY;
    case AC_MODE_DRY:  return EZB_ZCL_THERMOSTAT_SYSTEM_MODE_DRY;
    default:           return EZB_ZCL_THERMOSTAT_SYSTEM_MODE_OFF;
    }
}

static uint8_t fan_mode_for_state(const bridge_ac_state_t *state)
{
    if (!state->power) {
        return EZB_ZCL_FAN_CONTROL_FAN_MODE_OFF;
    }
    switch (state->fan) {
    case AC_FAN_LOW:         return EZB_ZCL_FAN_CONTROL_FAN_MODE_LOW;
    case AC_FAN_HIGH:        return EZB_ZCL_FAN_CONTROL_FAN_MODE_HIGH;
    default:                 return EZB_ZCL_FAN_CONTROL_FAN_MODE_OFF;
    }
}

static bool queue_control(const ac_control_request_t *request)
{
    /* PASSIVE is a hard read-only boundary while the jumpers are attached. */
    if (!bridge_service_mitm_active()) {
        ESP_LOGW(TAG, "rejecting Zigbee control in PASSIVE bridge profile");
        return false;
    }
    uint32_t sequence = 0;
    const esp_err_t err = bridge_service_queue_control(request, &sequence);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "control rejected by bridge: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "control queued sequence=%" PRIu32 " mask=0x%02" PRIx32, sequence, request->mask);
    return true;
}

static void setpoint_request(ezb_zcl_set_attr_value_message_t *message)
{
    if (message->in.attribute.data.type != EZB_ZCL_ATTR_TYPE_INT16 ||
        message->in.attribute.data.value == NULL) {
        message->out.result = EZB_ZCL_STATUS_INVALID_TYPE;
        return;
    }
    const int16_t value = *(const int16_t *)message->in.attribute.data.value;
    if (value < 1800 || value > 3200 || (value % 100) != 0) {
        message->out.result = EZB_ZCL_STATUS_INVALID_VALUE;
        return;
    }
    ac_control_request_t request = {
        .mask = AC_CONTROL_SETPOINT,
        .setpoint_c = (uint8_t)(value / 100),
    };
    message->out.result = queue_control(&request) ? EZB_ZCL_STATUS_SUCCESS : EZB_ZCL_STATUS_NOT_AUTHORIZED;
}

static void set_attr_value_handler(ezb_zcl_set_attr_value_message_t *message)
{
    if (message == NULL) {
        return;
    }
    message->out.result = EZB_ZCL_STATUS_UNSUP_ATTRIB;
    if (message->info.status != EZB_ZCL_STATUS_SUCCESS || message->info.dst_ep != ZIGBEE_ENDPOINT ||
        message->info.cluster_role != EZB_ZCL_CLUSTER_SERVER ||
        message->in.attribute.data.value == NULL) {
        return;
    }

    if (message->info.cluster_id == KLIMA_CLUSTER_ID) {
        const uint16_t attr = message->in.attribute.id;
        if (attr == KLIMA_ATTR_QUIET || attr == KLIMA_ATTR_UNITS_FAHRENHEIT) {
            if (message->in.attribute.data.type != EZB_ZCL_ATTR_TYPE_BOOL) {
                message->out.result = EZB_ZCL_STATUS_INVALID_TYPE;
                return;
            }
            const bool enabled = *(const bool *)message->in.attribute.data.value;
            ac_control_request_t request = {0};
            if (attr == KLIMA_ATTR_QUIET) {
                request.mask = AC_CONTROL_QUIET;
                request.quiet = enabled;
            } else {
                request.mask = AC_CONTROL_UNITS_FAHRENHEIT;
                request.units_fahrenheit = enabled;
            }
            message->out.result = queue_control(&request)
                ? EZB_ZCL_STATUS_SUCCESS : EZB_ZCL_STATUS_NOT_AUTHORIZED;
            return;
        }
        message->out.result = EZB_ZCL_STATUS_READ_ONLY;
        return;
    }

    if (message->info.cluster_id == EZB_ZCL_CLUSTER_ID_THERMOSTAT) {
        const uint16_t attr = message->in.attribute.id;
        if (attr == EZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID) {
            if (message->in.attribute.data.type != EZB_ZCL_ATTR_TYPE_ENUM8) {
                message->out.result = EZB_ZCL_STATUS_INVALID_TYPE;
                return;
            }
            const uint8_t mode = *(const uint8_t *)message->in.attribute.data.value;
            ac_control_request_t request = {0};
            switch (mode) {
            case EZB_ZCL_THERMOSTAT_SYSTEM_MODE_OFF:
                request.mask = AC_CONTROL_POWER;
                request.power = false;
                break;
            case EZB_ZCL_THERMOSTAT_SYSTEM_MODE_COOL:
                request.mask = AC_CONTROL_POWER | AC_CONTROL_MODE;
                request.power = true;
                request.mode = AC_MODE_COOL;
                break;
            case EZB_ZCL_THERMOSTAT_SYSTEM_MODE_FAN_ONLY:
                request.mask = AC_CONTROL_POWER | AC_CONTROL_MODE;
                request.power = true;
                request.mode = AC_MODE_FAN;
                break;
            case EZB_ZCL_THERMOSTAT_SYSTEM_MODE_DRY:
                request.mask = AC_CONTROL_POWER | AC_CONTROL_MODE;
                request.power = true;
                request.mode = AC_MODE_DRY;
                break;
            default:
                message->out.result = EZB_ZCL_STATUS_INVALID_VALUE;
                return;
            }
            message->out.result = queue_control(&request) ? EZB_ZCL_STATUS_SUCCESS : EZB_ZCL_STATUS_NOT_AUTHORIZED;
            return;
        }
        if (attr == EZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_COOLING_SETPOINT_ID ||
            attr == EZB_ZCL_ATTR_THERMOSTAT_UNOCCUPIED_COOLING_SETPOINT_ID) {
            setpoint_request(message);
            return;
        }
    }

    if (message->info.cluster_id == EZB_ZCL_CLUSTER_ID_FAN_CONTROL &&
        message->in.attribute.id == EZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID) {
        if (message->in.attribute.data.type != EZB_ZCL_ATTR_TYPE_ENUM8) {
            message->out.result = EZB_ZCL_STATUS_INVALID_TYPE;
            return;
        }
        const uint8_t mode = *(const uint8_t *)message->in.attribute.data.value;
        ac_control_request_t request = {0};
        if (mode == EZB_ZCL_FAN_CONTROL_FAN_MODE_LOW) {
            request.mask = AC_CONTROL_FAN;
            request.fan = AC_FAN_LOW;
        } else if (mode == EZB_ZCL_FAN_CONTROL_FAN_MODE_HIGH ||
                   mode == EZB_ZCL_FAN_CONTROL_FAN_MODE_AUTO) {
            request.mask = AC_CONTROL_FAN;
            request.fan = AC_FAN_HIGH;
        } else {
            message->out.result = EZB_ZCL_STATUS_INVALID_VALUE;
            return;
        }
        message->out.result = queue_control(&request) ? EZB_ZCL_STATUS_SUCCESS : EZB_ZCL_STATUS_NOT_AUTHORIZED;
    }
}

static void action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    if (callback_id == EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID) {
        set_attr_value_handler((ezb_zcl_set_attr_value_message_t *)message);
    }
}

static void add_basic_identity(ezb_af_ep_desc_t endpoint)
{
    ezb_zcl_cluster_desc_t basic = ezb_af_endpoint_get_cluster_desc(
        endpoint, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    if (basic == NULL) {
        return;
    }
    (void)ezb_zcl_basic_cluster_desc_add_attr(
        basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, s_manufacturer_name);
    (void)ezb_zcl_basic_cluster_desc_add_attr(
        basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, s_model_identifier);
    (void)ezb_zcl_basic_cluster_desc_add_attr(
        basic, EZB_ZCL_ATTR_BASIC_SW_BUILD_ID_ID, s_sw_build_id);
}

static esp_err_t add_custom_diagnostics(ezb_af_ep_desc_t endpoint)
{
    ezb_zcl_custom_cluster_config_t cfg = {.cluster_id = KLIMA_CLUSTER_ID};
    ezb_zcl_cluster_desc_t cluster = ezb_zcl_custom_create_cluster_desc(&cfg, EZB_ZCL_CLUSTER_SERVER);
    if (cluster == NULL) {
        return ESP_ERR_NO_MEM;
    }

#define ADD_DIAG(id, type, ptr) \
    do { \
        ezb_err_t add_err = ezb_zcl_custom_cluster_desc_add_manuf_attr( \
            cluster, (id), (type), EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_REPORTING, \
            KLIMA_MANUFACTURER_CODE, (ptr)); \
        if (add_err != EZB_ERR_NONE) return esp_zigbee_err_to_esp(add_err); \
    } while (0)
    ADD_DIAG(KLIMA_ATTR_CONTROL_AVAILABLE, EZB_ZCL_ATTR_TYPE_BOOL, &s_zigbee.control_available);
    ADD_DIAG(KLIMA_ATTR_RAW_MODE_FAN_CODE, EZB_ZCL_ATTR_TYPE_UINT8, &s_zigbee.raw_mode_fan_code);
    ADD_DIAG(KLIMA_ATTR_OPERATION_CODE, EZB_ZCL_ATTR_TYPE_UINT8, &s_zigbee.operation_code);
    ADD_DIAG(KLIMA_ATTR_MAIN_LINK_VALID, EZB_ZCL_ATTR_TYPE_BOOL, &s_zigbee.main_link_valid);
    ADD_DIAG(KLIMA_ATTR_PANEL_LINK_VALID, EZB_ZCL_ATTR_TYPE_BOOL, &s_zigbee.panel_link_valid);
    ADD_DIAG(KLIMA_ATTR_MAIN_AGE_MS, EZB_ZCL_ATTR_TYPE_INT32, &s_zigbee.main_age_ms);
    ADD_DIAG(KLIMA_ATTR_PANEL_AGE_MS, EZB_ZCL_ATTR_TYPE_INT32, &s_zigbee.panel_age_ms);
    ADD_DIAG(KLIMA_ATTR_SENSOR_1_RAW, EZB_ZCL_ATTR_TYPE_UINT8, &s_zigbee.sensor_1_raw);
    ADD_DIAG(KLIMA_ATTR_SENSOR_2_RAW, EZB_ZCL_ATTR_TYPE_UINT8, &s_zigbee.sensor_2_raw);
    ADD_DIAG(KLIMA_ATTR_MITM_ACTIVE, EZB_ZCL_ATTR_TYPE_BOOL, &s_zigbee.mitm_active);
    ADD_DIAG(KLIMA_ATTR_COMMAND_PENDING, EZB_ZCL_ATTR_TYPE_BOOL, &s_zigbee.command_pending);
    ADD_DIAG(KLIMA_ATTR_COMMAND_SEQUENCE, EZB_ZCL_ATTR_TYPE_UINT32, &s_zigbee.command_sequence);
    ADD_DIAG(KLIMA_ATTR_CHECKSUM_ERRORS, EZB_ZCL_ATTR_TYPE_UINT32, &s_zigbee.checksum_errors);
    ADD_DIAG(KLIMA_ATTR_FRAMING_ERRORS, EZB_ZCL_ATTR_TYPE_UINT32, &s_zigbee.framing_errors);
    ADD_DIAG(KLIMA_ATTR_INJECTED_FRAMES, EZB_ZCL_ATTR_TYPE_UINT32, &s_zigbee.injected_frames);
    ADD_DIAG(KLIMA_ATTR_PANEL_EVENT, EZB_ZCL_ATTR_TYPE_BOOL, &s_zigbee.panel_event);
    ADD_DIAG(KLIMA_ATTR_COMMAND_STATUS, EZB_ZCL_ATTR_TYPE_UINT8, &s_zigbee.command_status);
    ADD_DIAG(KLIMA_ATTR_COMMAND_TIMEOUTS, EZB_ZCL_ATTR_TYPE_UINT32, &s_zigbee.command_timeouts);
#define ADD_CONTROL(id, ptr) \
    do { \
        ezb_err_t add_err = ezb_zcl_custom_cluster_desc_add_manuf_attr( \
            cluster, (id), EZB_ZCL_ATTR_TYPE_BOOL, \
            EZB_ZCL_ATTR_ACCESS_READ | EZB_ZCL_ATTR_ACCESS_WRITE | EZB_ZCL_ATTR_ACCESS_REPORTING, \
            KLIMA_MANUFACTURER_CODE, (ptr)); \
        if (add_err != EZB_ERR_NONE) return esp_zigbee_err_to_esp(add_err); \
    } while (0)
    ADD_CONTROL(KLIMA_ATTR_QUIET, &s_zigbee.quiet);
    ADD_CONTROL(KLIMA_ATTR_UNITS_FAHRENHEIT, &s_zigbee.units_fahrenheit);
    ADD_DIAG(KLIMA_ATTR_TIMER, EZB_ZCL_ATTR_TYPE_BOOL, &s_zigbee.timer);
#undef ADD_CONTROL
#undef ADD_DIAG
    return esp_zigbee_err_to_esp(ezb_af_endpoint_add_cluster_desc(endpoint, cluster));
}

static esp_err_t add_fan_cluster(ezb_af_ep_desc_t endpoint)
{
    ezb_zcl_fan_control_cluster_server_config_t cfg = {
        .fan_mode = EZB_ZCL_FAN_CONTROL_FAN_MODE_OFF,
        .fan_mode_sequence = EZB_ZCL_FAN_CONTROL_FAN_MODE_SEQUENCE_LOW_HIGH_AUTO,
    };
    ezb_zcl_cluster_desc_t cluster = ezb_zcl_fan_control_create_cluster_desc(
        &cfg, EZB_ZCL_CLUSTER_SERVER);
    if (cluster == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return esp_zigbee_err_to_esp(ezb_af_endpoint_add_cluster_desc(endpoint, cluster));
}

static esp_err_t add_thermostat_setpoint_attrs(ezb_af_ep_desc_t endpoint)
{
    ezb_zcl_cluster_desc_t cluster = ezb_af_endpoint_get_cluster_desc(
        endpoint, EZB_ZCL_CLUSTER_ID_THERMOSTAT, EZB_ZCL_CLUSTER_SERVER);
    if (cluster == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    /* These are optional in the minimal ZHA thermostat factory on some 2.0.x
     * releases. Add them if absent; duplicate errors are harmless. */
    (void)ezb_zcl_thermostat_cluster_desc_add_attr(
        cluster, EZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_COOLING_SETPOINT_ID,
        &s_zigbee.occupied_cooling_setpoint);
    (void)ezb_zcl_thermostat_cluster_desc_add_attr(
        cluster, EZB_ZCL_ATTR_THERMOSTAT_UNOCCUPIED_COOLING_SETPOINT_ID,
        &s_zigbee.unoccupied_cooling_setpoint);
    return ESP_OK;
}

static esp_err_t register_data_model(void)
{
    ezb_zha_thermostat_config_t cfg = EZB_ZHA_THERMOSTAT_CONFIG();
    cfg.thermostat_cfg.system_mode = EZB_ZCL_THERMOSTAT_SYSTEM_MODE_OFF;
    ezb_af_ep_desc_t endpoint = ezb_zha_create_thermostat(ZIGBEE_ENDPOINT, &cfg);
    if (endpoint == NULL) {
        return ESP_ERR_NO_MEM;
    }
    add_basic_identity(endpoint);
    ESP_RETURN_ON_ERROR(add_fan_cluster(endpoint), TAG, "add FanControl cluster");
    ESP_RETURN_ON_ERROR(add_thermostat_setpoint_attrs(endpoint), TAG, "add thermostat setpoints");
    ESP_RETURN_ON_ERROR(add_custom_diagnostics(endpoint), TAG, "add Klima diagnostics cluster");
    ezb_zcl_core_action_handler_register(action_handler);
    ezb_af_device_desc_t device = ezb_af_create_device_desc();
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ezb_err_t add_err = ezb_af_device_add_endpoint_desc(device, endpoint);
    if (add_err != EZB_ERR_NONE) {
        return esp_zigbee_err_to_esp(add_err);
    }
    return esp_zigbee_err_to_esp(ezb_af_device_desc_register(device));
}

static void refresh_state_locked(const bridge_ac_state_t *state)
{
    s_zigbee.main_link_valid = state->main_valid;
    s_zigbee.panel_link_valid = state->panel_valid;
    s_zigbee.mitm_active = state->mitm_active;
    s_zigbee.control_available = state->mitm_active && state->main_valid && state->panel_valid;
    s_zigbee.command_pending = state->command_pending;
    s_zigbee.command_sequence = state->command_sequence;
    s_zigbee.command_status = (uint8_t)state->command_status;
    s_zigbee.command_timeouts = state->command_timeouts;
    s_zigbee.panel_event = state->panel_event;
    s_zigbee.quiet = (state->feature_flags & AC_FEATURE_QUIET) != 0U;
    s_zigbee.units_fahrenheit =
        (state->feature_flags & AC_FEATURE_UNITS_FAHRENHEIT) != 0U;
    s_zigbee.timer = (state->feature_flags & AC_FEATURE_TIMER) != 0U;
    s_zigbee.raw_mode_fan_code = state->mode_fan_code;
    s_zigbee.operation_code = state->operation_code;
    s_zigbee.sensor_1_raw = state->sensor_1_raw;
    s_zigbee.sensor_2_raw = state->sensor_2_raw;
    s_zigbee.main_age_ms = state->main_age_ms > INT32_MAX ? INT32_MAX : (int32_t)state->main_age_ms;
    s_zigbee.panel_age_ms = state->panel_age_ms > INT32_MAX ? INT32_MAX : (int32_t)state->panel_age_ms;
    s_zigbee.checksum_errors = state->checksum_errors;
    s_zigbee.framing_errors = state->framing_errors;
    s_zigbee.injected_frames = state->injected_frames;
    s_zigbee.system_mode = system_mode_for_state(state);
    s_zigbee.fan_mode = fan_mode_for_state(state);
    s_zigbee.local_temperature = state->mode == AC_MODE_COOL
        ? EZB_ZCL_VALUE_INT16_NaS
        : (int16_t)(state->main_display_temperature_c * 100);
    const uint8_t setpoint = (state->setpoint_c >= 18U && state->setpoint_c <= 32U) ? state->setpoint_c : 24U;
    s_zigbee.occupied_cooling_setpoint = (int16_t)(setpoint * 100);
    s_zigbee.unoccupied_cooling_setpoint = s_zigbee.occupied_cooling_setpoint;
    s_zigbee.running_mode = state->power ? s_zigbee.system_mode : EZB_ZCL_THERMOSTAT_THERMOSTAT_RUNNING_MODE_OFF;
    s_zigbee.fan_mode_sequence = EZB_ZCL_FAN_CONTROL_FAN_MODE_SEQUENCE_LOW_HIGH_AUTO;
    s_zigbee.rf_connected = s_zigbee.joined;
}

void zigbee_service_publish_state(void)
{
    if (!s_zigbee.started || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(100))) {
        return;
    }
    bridge_ac_state_t state;
    bridge_service_get_ac_state(&state);
    refresh_state_locked(&state);

    set_attr_report(ZIGBEE_ENDPOINT, EZB_ZCL_CLUSTER_ID_THERMOSTAT,
                    EZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_ID, EZB_ZCL_STD_MANUF_CODE,
                    &s_zigbee.local_temperature);
    set_attr_report(ZIGBEE_ENDPOINT, EZB_ZCL_CLUSTER_ID_THERMOSTAT,
                    EZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID, EZB_ZCL_STD_MANUF_CODE,
                    &s_zigbee.system_mode);
    set_attr_report(ZIGBEE_ENDPOINT, EZB_ZCL_CLUSTER_ID_THERMOSTAT,
                    EZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_COOLING_SETPOINT_ID, EZB_ZCL_STD_MANUF_CODE,
                    &s_zigbee.occupied_cooling_setpoint);
    set_attr_report(ZIGBEE_ENDPOINT, EZB_ZCL_CLUSTER_ID_THERMOSTAT,
                    EZB_ZCL_ATTR_THERMOSTAT_UNOCCUPIED_COOLING_SETPOINT_ID, EZB_ZCL_STD_MANUF_CODE,
                    &s_zigbee.unoccupied_cooling_setpoint);
    set_attr_report(ZIGBEE_ENDPOINT, EZB_ZCL_CLUSTER_ID_FAN_CONTROL,
                    EZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_ID, EZB_ZCL_STD_MANUF_CODE,
                    &s_zigbee.fan_mode);
    set_attr_report(ZIGBEE_ENDPOINT, EZB_ZCL_CLUSTER_ID_FAN_CONTROL,
                    EZB_ZCL_ATTR_FAN_CONTROL_FAN_MODE_SEQUENCE_ID, EZB_ZCL_STD_MANUF_CODE,
                    &s_zigbee.fan_mode_sequence);

#define REPORT_DIAG(id, ptr) set_attr_report(ZIGBEE_ENDPOINT, KLIMA_CLUSTER_ID, (id), KLIMA_MANUFACTURER_CODE, (ptr))
    REPORT_DIAG(KLIMA_ATTR_CONTROL_AVAILABLE, &s_zigbee.control_available);
    REPORT_DIAG(KLIMA_ATTR_RAW_MODE_FAN_CODE, &s_zigbee.raw_mode_fan_code);
    REPORT_DIAG(KLIMA_ATTR_OPERATION_CODE, &s_zigbee.operation_code);
    REPORT_DIAG(KLIMA_ATTR_MAIN_LINK_VALID, &s_zigbee.main_link_valid);
    REPORT_DIAG(KLIMA_ATTR_PANEL_LINK_VALID, &s_zigbee.panel_link_valid);
    REPORT_DIAG(KLIMA_ATTR_MAIN_AGE_MS, &s_zigbee.main_age_ms);
    REPORT_DIAG(KLIMA_ATTR_PANEL_AGE_MS, &s_zigbee.panel_age_ms);
    REPORT_DIAG(KLIMA_ATTR_SENSOR_1_RAW, &s_zigbee.sensor_1_raw);
    REPORT_DIAG(KLIMA_ATTR_SENSOR_2_RAW, &s_zigbee.sensor_2_raw);
    REPORT_DIAG(KLIMA_ATTR_MITM_ACTIVE, &s_zigbee.mitm_active);
    REPORT_DIAG(KLIMA_ATTR_COMMAND_PENDING, &s_zigbee.command_pending);
    REPORT_DIAG(KLIMA_ATTR_COMMAND_SEQUENCE, &s_zigbee.command_sequence);
    REPORT_DIAG(KLIMA_ATTR_CHECKSUM_ERRORS, &s_zigbee.checksum_errors);
    REPORT_DIAG(KLIMA_ATTR_FRAMING_ERRORS, &s_zigbee.framing_errors);
    REPORT_DIAG(KLIMA_ATTR_INJECTED_FRAMES, &s_zigbee.injected_frames);
    REPORT_DIAG(KLIMA_ATTR_PANEL_EVENT, &s_zigbee.panel_event);
    REPORT_DIAG(KLIMA_ATTR_COMMAND_STATUS, &s_zigbee.command_status);
    REPORT_DIAG(KLIMA_ATTR_COMMAND_TIMEOUTS, &s_zigbee.command_timeouts);
    REPORT_DIAG(KLIMA_ATTR_QUIET, &s_zigbee.quiet);
    REPORT_DIAG(KLIMA_ATTR_UNITS_FAHRENHEIT, &s_zigbee.units_fahrenheit);
    REPORT_DIAG(KLIMA_ATTR_TIMER, &s_zigbee.timer);
#undef REPORT_DIAG
    esp_zigbee_lock_release();
}

static void commissioning_start(ezb_bdb_comm_mode_mask_t mode)
{
    const ezb_err_t err = ezb_bdb_start_top_level_commissioning(mode);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "commissioning mode 0x%02x failed: %d", mode, err);
    }
}

static bool signal_handler(const ezb_app_signal_t *signal)
{
    const ezb_app_signal_type_t type = ezb_app_signal_get_type(signal);
    const ezb_bdb_signal_simple_params_t *params =
        (const ezb_bdb_signal_simple_params_t *)ezb_app_signal_get_params(signal);
    const uint8_t status = params != NULL ? params->status : EZB_BDB_STATUS_SUCCESS;
    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        commissioning_start(EZB_BDB_MODE_INITIALIZATION);
        return true;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT:
        s_zigbee.joined = false;
        if (status == EZB_BDB_STATUS_SUCCESS) {
            commissioning_start(EZB_BDB_MODE_NETWORK_STEERING);
        }
        return true;
    case EZB_BDB_SIGNAL_STEERING:
        if (status == EZB_BDB_STATUS_SUCCESS) {
            s_zigbee.joined = true;
            ezb_set_rx_on_when_idle(true); /* always-powered ZED */
            ESP_LOGI(TAG, "joined PAN=0x%04x channel=%u short=0x%04x",
                     ezb_get_panid(), ezb_get_current_channel(), ezb_get_short_address());
            zigbee_service_publish_state();
        } else {
            s_zigbee.joined = false;
            commissioning_start(EZB_BDB_MODE_NETWORK_STEERING);
        }
        return true;
    case EZB_ZDO_SIGNAL_LEAVE:
        s_zigbee.joined = false;
        commissioning_start(EZB_BDB_MODE_NETWORK_STEERING);
        return true;
    default:
        return false;
    }
}

static void zigbee_main_task(void *arg)
{
    (void)arg;
    esp_zigbee_config_t config = {
        .device_config = {
            .device_type = EZB_NWK_DEVICE_TYPE_END_DEVICE,
            .install_code_policy = false,
            .zed_config = {
                .ed_timeout = EZB_NWK_ED_TIMEOUT_64MIN,
                .keep_alive = 10000,
            },
        },
        .platform_config = {
            .storage_partition_name = "zb_storage",
            .radio_config = {.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE},
        },
    };
    esp_err_t err = nvs_flash_init_partition("zb_storage");
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition("zb_storage"));
        err = nvs_flash_init_partition("zb_storage");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "zb_storage init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_bdb_set_primary_channel_set(ZIGBEE_PRIMARY_CHANNEL_MASK)));
    ESP_ERROR_CHECK(register_data_model());
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_app_signal_add_handler(signal_handler)));
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    ezb_set_rx_on_when_idle(true);
    s_zigbee.started = true;
    ESP_LOGI(TAG, "native always-powered Zigbee End Device started on endpoint %u", ZIGBEE_ENDPOINT);
    ESP_ERROR_CHECK(esp_zigbee_launch_mainloop());
    s_zigbee.started = false;
    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

static void report_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        zigbee_service_publish_state();
    }
}

esp_err_t zigbee_service_start(void)
{
    memset(&s_zigbee, 0, sizeof(s_zigbee));
    const esp_app_desc_t *app = esp_app_get_description();
    const size_t version_len = strnlen(app->version, sizeof(s_sw_build_id) - 1U);
    s_sw_build_id[0] = (uint8_t)version_len;
    memcpy(&s_sw_build_id[1], app->version, version_len);
    if (xTaskCreate(zigbee_main_task, "zigbee_main", 8192, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(report_task, "zigbee_report", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool zigbee_service_is_joined(void)
{
    return s_zigbee.started && s_zigbee.joined;
}
