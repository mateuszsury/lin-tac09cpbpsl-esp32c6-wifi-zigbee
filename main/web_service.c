#include "web_service.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "bridge_service.h"
#include "capture_store.h"
#include "klima_secrets.h"
#include "telemetry.h"
#include "web_ui.h"
#include "wifi_service.h"

#ifndef KLIMA_NTS0104_PREFLIGHT
#define KLIMA_NTS0104_PREFLIGHT 0
#endif
#if KLIMA_NTS0104_PREFLIGHT
#include "nts0104_preflight.h"
#endif

#define JSON_BUFFER_SIZE 16384U
#define LOGS_PER_RESPONSE 12U
#define OTA_CHUNK_SIZE 4096U
#define MARKER_MAX_BODY_SIZE 192U
#define MARKER_MAX_LABEL_SIZE 96U

static httpd_handle_t s_server;

static void bytes_to_hex(char *output, size_t output_size, const uint8_t *data, size_t length);

static bool secure_token_equal(const char *candidate)
{
    const size_t expected_length = strlen(KLIMA_DEVICE_TOKEN);
    const size_t candidate_length = candidate ? strlen(candidate) : 0U;
    unsigned difference = (unsigned)(expected_length ^ candidate_length);
    const size_t compare_length = expected_length > candidate_length
        ? expected_length
        : candidate_length;
    for (size_t i = 0; i < compare_length; ++i) {
        const unsigned expected = i < expected_length ? (unsigned char)KLIMA_DEVICE_TOKEN[i] : 0U;
        const unsigned actual = i < candidate_length ? (unsigned char)candidate[i] : 0U;
        difference |= expected ^ actual;
    }
    return difference == 0U;
}

static bool request_authorized(httpd_req_t *request)
{
    /* Unconfigured public builds must not accept mutating API calls. */
    const size_t expected_length = strlen(KLIMA_DEVICE_TOKEN);
    if (expected_length < 24U || expected_length >= 128U) {
        return false;
    }
    const size_t header_length = httpd_req_get_hdr_value_len(request, "X-Klima-Token");
    if (header_length == 0U || header_length >= 128U) {
        return false;
    }
    char token[128];
    if (httpd_req_get_hdr_value_str(request, "X-Klima-Token", token, sizeof(token)) != ESP_OK) {
        return false;
    }
    return secure_token_equal(token);
}

static esp_err_t unauthorized(httpd_req_t *request)
{
    httpd_resp_set_status(request, "401 Unauthorized");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"error\":\"invalid device token\"}");
}

static bool marker_label_valid(const char *label)
{
    if (!label) {
        return false;
    }
    const size_t length = strlen(label);
    if (length == 0U || length >= MARKER_MAX_LABEL_SIZE) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const unsigned char value = (unsigned char)label[i];
        if (value < 0x20U || value == ',' || value == '\\' || value == '"') {
            return false;
        }
    }
    return true;
}

static esp_err_t root_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, KLIMA_WEB_UI, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *request)
{
    wifi_service_status_t wifi_status;
    wifi_service_get_status(&wifi_status);
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    const esp_partition_t *capture_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "capture");
    const esp_partition_t *zigbee_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "zb_storage");
    capture_store_info_t capture;
    capture_store_get_info(&capture);
#if KLIMA_NTS0104_PREFLIGHT
    nts0104_preflight_status_t preflight;
    nts0104_preflight_get_status(&preflight);
#endif

    char response[2048];
    snprintf(
        response,
        sizeof(response),
        "{\"version\":\"%s\",\"sta_connected\":%s,\"sta_ip\":\"%s\","
        "\"ap_enabled\":%s,\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\",\"ota_partition\":\"%s\","
        "\"free_heap\":%" PRIu32
        ",\"uptime_s\":%.3f,\"latest_sequence\":%" PRIu64 ","
        "\"partitions\":{\"capture_available\":%s,\"capture_offset\":%" PRIu32
        ",\"capture_size\":%" PRIu32 ",\"zigbee_storage_available\":%s,"
        "\"zigbee_storage_offset\":%" PRIu32 ",\"zigbee_storage_size\":%" PRIu32 "},"
        "\"capture\":{\"available\":%s,\"valid_records\":%" PRIu32
        ",\"capacity_records\":%" PRIu32 ",\"latest_sequence\":%" PRIu32
        ",\"dropped_records\":%" PRIu32 ",\"write_errors\":%" PRIu32 "},"
        "\"preflight\":{\"initialized\":%s,\"oe_enabled\":%s,"
        "\"supply_contract_valid\":%s,\"self_test_passed\":%s,"
        "\"reason\":\"%s\",\"vdda_mv\":%" PRIu32 ",\"vddb_mv\":%" PRIu32 "}}",
        app->version,
        wifi_status.sta_connected ? "true" : "false",
        wifi_status.sta_ip,
        wifi_status.ap_enabled ? "true" : "false",
        wifi_status.ap_enabled ? wifi_status.ap_ssid : "",
        wifi_status.ap_enabled ? "192.168.4.1" : "",
        running_partition ? running_partition->label : "unknown",
        esp_get_free_heap_size(),
        esp_timer_get_time() / 1000000.0,
        telemetry_latest_sequence(),
        capture_partition ? "true" : "false",
        capture_partition ? capture_partition->address : 0U,
        capture_partition ? capture_partition->size : 0U,
        zigbee_partition ? "true" : "false",
        zigbee_partition ? zigbee_partition->address : 0U,
        zigbee_partition ? zigbee_partition->size : 0U,
        capture.available ? "true" : "false",
        capture.valid_records,
        capture.capacity_records,
        capture.latest_sequence,
        capture.dropped_records,
        capture.write_errors,
#if KLIMA_NTS0104_PREFLIGHT
        preflight.initialized ? "true" : "false",
        preflight.oe_enabled ? "true" : "false",
        preflight.supply_contract_valid ? "true" : "false",
        preflight.self_test_passed ? "true" : "false",
        nts0104_preflight_reason_name(preflight.reason),
        preflight.vdda_mv,
        preflight.vddb_mv
#else
        "false", "false", "false", "false", "not_compiled", 0U, 0U
#endif
        );
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, response);
}

static bool append_char(char *buffer, size_t capacity, size_t *length, char value)
{
    if (*length + 1U >= capacity) {
        return false;
    }
    buffer[(*length)++] = value;
    buffer[*length] = '\0';
    return true;
}

static bool append_text(char *buffer, size_t capacity, size_t *length, const char *text)
{
    while (*text) {
        if (!append_char(buffer, capacity, length, *text++)) {
            return false;
        }
    }
    return true;
}

static bool append_json_escaped(char *buffer, size_t capacity, size_t *length, const char *text)
{
    for (; *text; ++text) {
        const unsigned char value = (unsigned char)*text;
        if (value == '"' || value == '\\') {
            if (!append_char(buffer, capacity, length, '\\') ||
                !append_char(buffer, capacity, length, (char)value)) {
                return false;
            }
        } else if (value == '\n' || value == '\r' || value == '\t') {
            const char escaped = value == '\n' ? 'n' : (value == '\r' ? 'r' : 't');
            if (!append_char(buffer, capacity, length, '\\') ||
                !append_char(buffer, capacity, length, escaped)) {
                return false;
            }
        } else if (value >= 0x20U) {
            if (!append_char(buffer, capacity, length, (char)value)) {
                return false;
            }
        }
    }
    return true;
}

static uint64_t query_after(httpd_req_t *request)
{
    const size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length == 0U || query_length >= 128U) {
        return 0U;
    }
    char query[128];
    char value[32];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "after", value, sizeof(value)) != ESP_OK) {
        return 0U;
    }
    return strtoull(value, NULL, 10);
}

static esp_err_t logs_handler(httpd_req_t *request)
{
    char *response = calloc(1U, JSON_BUFFER_SIZE);
    if (!response) {
        return httpd_resp_send_500(request);
    }

    size_t length = 0U;
    uint64_t after = query_after(request);
    append_text(response, JSON_BUFFER_SIZE, &length, "{\"entries\":[");

    size_t emitted = 0U;
    telemetry_entry_t entry;
    while (emitted < LOGS_PER_RESPONSE && telemetry_get_after(after, &entry)) {
        char prefix[80];
        snprintf(
            prefix,
            sizeof(prefix),
            "%s{\"seq\":%" PRIu64 ",\"text\":\"",
            emitted ? "," : "",
            entry.sequence);
        if (!append_text(response, JSON_BUFFER_SIZE, &length, prefix) ||
            !append_json_escaped(response, JSON_BUFFER_SIZE, &length, entry.text) ||
            !append_text(response, JSON_BUFFER_SIZE, &length, "\"}")) {
            break;
        }
        after = entry.sequence;
        ++emitted;
    }

    char suffix[80];
    snprintf(suffix, sizeof(suffix), "],\"latest\":%" PRIu64 "}", telemetry_latest_sequence());
    append_text(response, JSON_BUFFER_SIZE, &length, suffix);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const esp_err_t result = httpd_resp_send(request, response, length);
    free(response);
    return result;
}

static esp_err_t marker_handler(httpd_req_t *request)
{
    if (!request_authorized(request)) {
        return unauthorized(request);
    }
    if (request->content_len <= 0 || request->content_len >= MARKER_MAX_BODY_SIZE) {
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_sendstr(request, "{\"error\":\"invalid marker body\"}");
    }
    char body[MARKER_MAX_BODY_SIZE] = {0};
    size_t received = 0U;
    while (received < (size_t)request->content_len) {
        const int result = httpd_req_recv(
            request,
            body + received,
            (size_t)request->content_len - received);
        if (result <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)result;
    }
    cJSON *root = cJSON_ParseWithLength(body, received);
    const cJSON *label = root ? cJSON_GetObjectItemCaseSensitive(root, "label") : NULL;
    if (!cJSON_IsString(label) || !marker_label_valid(label->valuestring)) {
        cJSON_Delete(root);
        httpd_resp_set_status(request, "400 Bad Request");
        httpd_resp_set_type(request, "application/json");
        return httpd_resp_sendstr(request, "{\"error\":\"label must be 1-95 safe characters\"}");
    }
    const esp_err_t persisted = capture_store_record_marker(label->valuestring);
    telemetry_publishf(
        "MARKER,t_us=%" PRId64 ",label=%s",
        esp_timer_get_time(),
        label->valuestring);
    cJSON_Delete(root);
    if (persisted != ESP_OK) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_set_type(request, "application/json");
        return httpd_resp_sendstr(request, "{\"error\":\"marker was not persisted\"}");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, "{\"ok\":true}");
}

static const char *capture_direction_name(capture_direction_t direction)
{
    switch (direction) {
    case CAPTURE_DIRECTION_PANEL_TO_MAIN:
        return "panel_to_main";
    case CAPTURE_DIRECTION_MAIN_TO_PANEL:
        return "main_to_panel";
    default:
        return "unknown";
    }
}

static const char *capture_kind_name(capture_kind_t kind)
{
    switch (kind) {
    case CAPTURE_KIND_INITIAL:
        return "initial";
    case CAPTURE_KIND_STATE_CHANGE:
        return "state_change";
    case CAPTURE_KIND_EVENT:
        return "event";
    case CAPTURE_KIND_WIFI_COMMAND:
        return "wifi_command";
    case CAPTURE_KIND_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

static void capture_record_text(
    const capture_store_record_t *record,
    char *text,
    size_t capacity)
{
    if (record->type == CAPTURE_RECORD_FRAME) {
        char hex[CAPTURE_STORE_DATA_MAX * 2U + 1U] = {0};
        bytes_to_hex(hex, sizeof(hex), record->data, record->length);
        snprintf(
            text,
            capacity,
            "FRAME,t_us=%" PRId64 ",direction=%s,kind=%s,len=%u,hex=%s",
            record->timestamp_us,
            capture_direction_name(record->direction),
            capture_kind_name(record->kind),
            (unsigned)record->length,
            hex);
        return;
    }

    char detail[CAPTURE_STORE_DATA_MAX + 1U] = {0};
    memcpy(detail, record->data, record->length);
    if (record->type == CAPTURE_RECORD_MARKER) {
        snprintf(text, capacity, "MARKER,t_us=%" PRId64 ",label=%s", record->timestamp_us, detail);
    } else if (record->type == CAPTURE_RECORD_BOOT) {
        snprintf(text, capacity, "CAPTURE,t_us=%" PRId64 ",event=boot,detail=%s", record->timestamp_us, detail);
    } else {
        snprintf(text, capacity, "CAPTURE,t_us=%" PRId64 ",event=system,detail=%s", record->timestamp_us, detail);
    }
}

static esp_err_t capture_status_handler(httpd_req_t *request)
{
    capture_store_info_t info;
    capture_store_get_info(&info);
    char response[320];
    snprintf(
        response,
        sizeof(response),
        "{\"available\":%s,\"capacity_records\":%" PRIu32
        ",\"valid_records\":%" PRIu32 ",\"oldest_sequence\":%" PRIu32
        ",\"latest_sequence\":%" PRIu32 ",\"dropped_records\":%" PRIu32
        ",\"write_errors\":%" PRIu32 "}",
        info.available ? "true" : "false",
        info.capacity_records,
        info.valid_records,
        info.oldest_sequence,
        info.latest_sequence,
        info.dropped_records,
        info.write_errors);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, response);
}

static esp_err_t capture_export_handler(httpd_req_t *request)
{
    if (!request_authorized(request)) {
        return unauthorized(request);
    }
    const uint64_t requested_after = query_after(request);
    uint32_t after = requested_after > UINT32_MAX
        ? UINT32_MAX
        : (uint32_t)requested_after;
    httpd_resp_set_type(request, "application/x-ndjson; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Content-Disposition", "attachment; filename=klima-capture.ndjson");

    capture_store_record_t record;
    while (capture_store_get_after(after, &record)) {
        char text[320] = {0};
        char line[512] = {0};
        size_t length = 0U;
        capture_record_text(&record, text, sizeof(text));
        char prefix[96];
        snprintf(
            prefix,
            sizeof(prefix),
            "{\"sequence\":%" PRIu32 ",\"device_time_us\":%" PRId64 ",\"text\":\"",
            record.sequence,
            record.timestamp_us);
        if (!append_text(line, sizeof(line), &length, prefix) ||
            !append_json_escaped(line, sizeof(line), &length, text) ||
            !append_text(line, sizeof(line), &length, "\"}\n")) {
            return ESP_FAIL;
        }
        if (httpd_resp_send_chunk(request, line, length) != ESP_OK) {
            return ESP_FAIL;
        }
        after = record.sequence;
    }
    return httpd_resp_send_chunk(request, NULL, 0U);
}

static void bytes_to_hex(char *output, size_t output_size, const uint8_t *data, size_t length)
{
    size_t offset = 0U;
    for (size_t i = 0; i < length && offset + 2U < output_size; ++i) {
        const int written = snprintf(output + offset, output_size - offset, "%02X", data[i]);
        if (written <= 0) {
            break;
        }
        offset += (size_t)written;
    }
}

static esp_err_t ac_state_handler(httpd_req_t *request)
{
    bridge_ac_state_t state = {0};
    bridge_service_get_ac_state(&state);
    char main_hex[sizeof(state.main_raw) * 2U + 1U] = {0};
    char panel_hex[sizeof(state.panel_raw) * 2U + 1U] = {0};
    char forwarded_hex[sizeof(state.forwarded_panel_raw) * 2U + 1U] = {0};
    char b4_hex[sizeof(state.b4_monitor_raw) * 2U + 1U] = {0};
    bytes_to_hex(main_hex, sizeof(main_hex), state.main_raw, sizeof(state.main_raw));
    bytes_to_hex(panel_hex, sizeof(panel_hex), state.panel_raw, sizeof(state.panel_raw));
    bytes_to_hex(
        forwarded_hex,
        sizeof(forwarded_hex),
        state.forwarded_panel_raw,
        sizeof(state.forwarded_panel_raw));
    bytes_to_hex(b4_hex, sizeof(b4_hex), state.b4_monitor_raw, sizeof(state.b4_monitor_raw));

    char response[2048];
    snprintf(
        response,
        sizeof(response),
        "{\"profile\":\"%s\",\"link\":{\"main\":%s,\"panel\":%s,"
        "\"main_age_ms\":%" PRId64 ",\"panel_age_ms\":%" PRId64 "},"
        "\"state\":{\"power\":%s,\"feature_flags\":\"%02X\",\"quiet\":%s,"
        "\"units_fahrenheit\":%s,\"timer\":%s,\"setpoint_c\":%u,"
        "\"main_display_temperature_c\":%u,\"main_display_temperature_encoded\":%u,\"mode\":\"%s\","
        "\"fan\":\"%s\",\"mode_fan_code\":\"%02X\",\"operation_code\":\"%02X\","
        "\"sensor_1_raw\":%u,\"sensor_2_raw\":%u,\"panel_event\":%s,"
        "\"panel_emulated\":%s},"
        "\"control\":{\"available\":%s,\"override_active\":%s,\"pending\":%s,"
        "\"sequence\":%" PRIu32 ",\"status\":\"%s\",\"timeouts\":%" PRIu32 "},"
        "\"counters\":{\"main_frames\":%" PRIu32 ",\"panel_frames\":%" PRIu32
        ",\"main_rx_bytes\":%" PRIu32 ",\"panel_rx_bytes\":%" PRIu32
        ",\"main_rx_level\":%d,\"panel_rx_level\":%d"
        ",\"alternate_panel_rx_level\":%d,\"panel_candidate_gpio5_level\":%d"
        ",\"panel_tx_pad_level\":%d,\"main_tx_pad_level\":%d"
        ",\"mirror_main_edges\":%" PRIu32 ",\"alternate_panel_edges\":%" PRIu32
        ",\"panel_candidate_gpio5_edges\":%" PRIu32
        ",\"panel_tx_pad_edges\":%" PRIu32 ",\"main_tx_pad_edges\":%" PRIu32
        ",\"a4_monitor_level\":%d,\"b4_monitor_level\":%d"
        ",\"a4_monitor_edges\":%" PRIu32 ",\"b4_monitor_edges\":%" PRIu32
        ",\"b4_monitor_bytes\":%" PRIu32 ",\"b4_monitor_frames\":%" PRIu32
        ",\"b4_monitor_checksum_errors\":%" PRIu32
        ",\"b4_low_last_us\":%" PRIu32 ",\"b4_low_min_us\":%" PRIu32
        ",\"b4_low_max_us\":%" PRIu32 ",\"b4_capture_lateness_us\":%" PRIu32
        ",\"checksum_errors\":%" PRIu32 ",\"framing_errors\":%" PRIu32
        ",\"injected_frames\":%" PRIu32 "},\"raw\":{\"main\":\"%s\","
        "\"panel\":\"%s\",\"forwarded_panel\":\"%s\",\"b4_monitor\":\"%s\"}}",
        bridge_service_profile_name(),
        state.main_valid ? "true" : "false",
        state.panel_valid ? "true" : "false",
        state.main_age_ms,
        state.panel_age_ms,
        state.power ? "true" : "false",
        state.feature_flags,
        (state.feature_flags & AC_FEATURE_QUIET) != 0U ? "true" : "false",
        (state.feature_flags & AC_FEATURE_UNITS_FAHRENHEIT) != 0U ? "true" : "false",
        (state.feature_flags & AC_FEATURE_TIMER) != 0U ? "true" : "false",
        state.setpoint_c,
        state.main_display_temperature_c,
        state.main_display_temperature_encoded,
        ac_protocol_mode_name(state.mode),
        ac_protocol_fan_name(state.fan),
        state.mode_fan_code,
        state.operation_code,
        state.sensor_1_raw,
        state.sensor_2_raw,
        state.panel_event ? "true" : "false",
        state.panel_emulated ? "true" : "false",
        state.mitm_active && state.main_valid && state.panel_valid ? "true" : "false",
        state.override_active ? "true" : "false",
        state.command_pending ? "true" : "false",
        state.command_sequence,
        bridge_service_command_status_name(state.command_status),
        state.command_timeouts,
        state.main_frames,
        state.panel_frames,
        state.main_rx_bytes,
        state.panel_rx_bytes,
        state.main_rx_level,
        state.panel_rx_level,
        state.alternate_panel_rx_level,
        state.panel_candidate_gpio5_level,
        state.panel_tx_pad_level,
        state.main_tx_pad_level,
        state.mirror_main_edges,
        state.alternate_panel_edges,
        state.panel_candidate_gpio5_edges,
        state.panel_tx_pad_edges,
        state.main_tx_pad_edges,
        state.a4_monitor_level,
        state.b4_monitor_level,
        state.a4_monitor_edges,
        state.b4_monitor_edges,
        state.b4_monitor_bytes,
        state.b4_monitor_frames,
        state.b4_monitor_checksum_errors,
        state.b4_low_last_us,
        state.b4_low_min_us,
        state.b4_low_max_us,
        state.b4_capture_lateness_us,
        state.checksum_errors,
        state.framing_errors,
        state.injected_frames,
        main_hex,
        panel_hex,
        forwarded_hex,
        b4_hex);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, response);
}

static esp_err_t json_error(httpd_req_t *request, const char *status, const char *message)
{
    char response[192];
    snprintf(response, sizeof(response), "{\"error\":\"%s\"}", message);
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, response);
}

static esp_err_t read_json_body(httpd_req_t *request, char *body, size_t capacity)
{
    if (request->content_len <= 0 || (size_t)request->content_len >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    int offset = 0;
    while (offset < request->content_len) {
        const int received = httpd_req_recv(
            request,
            body + offset,
            request->content_len - offset);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            return ESP_FAIL;
        }
        offset += received;
    }
    body[offset] = '\0';
    return ESP_OK;
}

static bool parse_mode(const char *value, ac_mode_t *mode)
{
    if (strcmp(value, "cool") == 0) {
        *mode = AC_MODE_COOL;
    } else if (strcmp(value, "fan") == 0) {
        *mode = AC_MODE_FAN;
    } else if (strcmp(value, "dry") == 0) {
        *mode = AC_MODE_DRY;
    } else {
        return false;
    }
    return true;
}

static bool parse_fan(const char *value, ac_fan_t *fan)
{
    if (strcmp(value, "low") == 0) {
        *fan = AC_FAN_LOW;
    } else if (strcmp(value, "high") == 0) {
        *fan = AC_FAN_HIGH;
    } else {
        return false;
    }
    return true;
}

static esp_err_t control_handler(httpd_req_t *request)
{
    if (!request_authorized(request)) {
        return unauthorized(request);
    }
    if (!bridge_service_mitm_active()) {
        return json_error(
            request,
            "409 Conflict",
            "control disabled in safe-passive build; verify both UART links before installing MITM image");
    }

    char body[512];
    if (read_json_body(request, body, sizeof(body)) != ESP_OK) {
        return json_error(request, "400 Bad Request", "invalid request body");
    }
    cJSON *root = cJSON_Parse(body);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return json_error(request, "400 Bad Request", "invalid JSON");
    }

    ac_control_request_t control = {0};
    const cJSON *power = cJSON_GetObjectItemCaseSensitive(root, "power");
    const cJSON *setpoint = cJSON_GetObjectItemCaseSensitive(root, "setpoint_c");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    const cJSON *fan = cJSON_GetObjectItemCaseSensitive(root, "fan");
    const cJSON *raw = cJSON_GetObjectItemCaseSensitive(root, "raw_mode_fan_code");
    const cJSON *quiet = cJSON_GetObjectItemCaseSensitive(root, "quiet");
    const cJSON *units_fahrenheit = cJSON_GetObjectItemCaseSensitive(root, "units_fahrenheit");
    const cJSON *timer = cJSON_GetObjectItemCaseSensitive(root, "timer");
    bool valid = true;

    if (timer) {
        cJSON_Delete(root);
        return json_error(
            request,
            "400 Bad Request",
            "timer duration is not decoded; timer control is read-only");
    }

    if (power) {
        valid = cJSON_IsBool(power);
        if (valid) {
            control.mask |= AC_CONTROL_POWER;
            control.power = cJSON_IsTrue(power);
        }
    }
    if (valid && setpoint) {
        valid = cJSON_IsNumber(setpoint) && setpoint->valuedouble == setpoint->valueint &&
            setpoint->valueint >= 18 && setpoint->valueint <= 32;
        if (valid) {
            control.mask |= AC_CONTROL_SETPOINT;
            control.setpoint_c = (uint8_t)setpoint->valueint;
        }
    }
    if (valid && mode) {
        valid = cJSON_IsString(mode) && mode->valuestring &&
            parse_mode(mode->valuestring, &control.mode);
        if (valid) {
            control.mask |= AC_CONTROL_MODE;
        }
    }
    if (valid && fan) {
        valid = cJSON_IsString(fan) && fan->valuestring &&
            parse_fan(fan->valuestring, &control.fan);
        if (valid) {
            control.mask |= AC_CONTROL_FAN;
        }
    }
    if (valid && raw) {
        valid = cJSON_IsNumber(raw) && raw->valuedouble == raw->valueint &&
            raw->valueint >= 0 && raw->valueint <= 255;
        if (valid) {
            control.mask |= AC_CONTROL_RAW_MODE_FAN;
            control.raw_mode_fan_code = (uint8_t)raw->valueint;
        }
    }
    if (valid && quiet) {
        valid = cJSON_IsBool(quiet);
        if (valid) {
            control.mask |= AC_CONTROL_QUIET;
            control.quiet = cJSON_IsTrue(quiet);
        }
    }
    if (valid && units_fahrenheit) {
        valid = cJSON_IsBool(units_fahrenheit);
        if (valid) {
            control.mask |= AC_CONTROL_UNITS_FAHRENHEIT;
            control.units_fahrenheit = cJSON_IsTrue(units_fahrenheit);
        }
    }
    cJSON_Delete(root);

    if (!valid || control.mask == 0U ||
        ((control.mask & AC_CONTROL_RAW_MODE_FAN) != 0U &&
         (control.mask & (AC_CONTROL_MODE | AC_CONTROL_FAN)) != 0U)) {
        return json_error(request, "400 Bad Request", "unsupported or conflicting control fields");
    }

    uint32_t sequence = 0U;
    const esp_err_t result = bridge_service_queue_control(&control, &sequence);
    if (result == ESP_ERR_INVALID_STATE) {
        return json_error(request, "503 Service Unavailable", "panel link is not ready");
    }
    if (result != ESP_OK) {
        return json_error(request, "400 Bad Request", "control request rejected");
    }

    char response[96];
    snprintf(response, sizeof(response), "{\"ok\":true,\"sequence\":%" PRIu32 "}", sequence);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, response);
}

static void delayed_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t ota_handler(httpd_req_t *request)
{
    if (!request_authorized(request)) {
        return unauthorized(request);
    }
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        return httpd_resp_send_500(request);
    }
    if (request->content_len <= 0 || request->content_len > partition->size) {
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_sendstr(request, "{\"error\":\"invalid image size\"}");
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t result = esp_ota_begin(partition, request->content_len, &ota_handle);
    if (result != ESP_OK) {
        telemetry_publishf("OTA,event=begin_error,code=%s", esp_err_to_name(result));
        return httpd_resp_send_500(request);
    }

    uint8_t *buffer = malloc(OTA_CHUNK_SIZE);
    if (!buffer) {
        esp_ota_abort(ota_handle);
        return httpd_resp_send_500(request);
    }

    int remaining = request->content_len;
    int written = 0;
    while (remaining > 0) {
        const int wanted = remaining > OTA_CHUNK_SIZE ? OTA_CHUNK_SIZE : remaining;
        int received = httpd_req_recv(request, (char *)buffer, wanted);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            result = ESP_FAIL;
            break;
        }
        result = esp_ota_write(ota_handle, buffer, received);
        if (result != ESP_OK) {
            break;
        }
        written += received;
        remaining -= received;
    }
    free(buffer);

    if (result == ESP_OK) {
        result = esp_ota_end(ota_handle);
    } else {
        esp_ota_abort(ota_handle);
    }
    if (result == ESP_OK) {
        result = esp_ota_set_boot_partition(partition);
    }
    if (result != ESP_OK) {
        telemetry_publishf(
            "OTA,event=failed,written=%d,expected=%d,code=%s",
            written,
            request->content_len,
            esp_err_to_name(result));
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_sendstr(request, "{\"error\":\"OTA validation failed\"}");
    }

    telemetry_publishf(
        "OTA,event=accepted,bytes=%d,partition=%s,restart=1",
        written,
        partition->label);
    (void)capture_store_record_system("ota_accepted");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_sendstr(request, "{\"ok\":true,\"restarting\":true}");
    xTaskCreate(delayed_restart_task, "ota_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t reboot_handler(httpd_req_t *request)
{
    if (!request_authorized(request)) {
        return unauthorized(request);
    }
    telemetry_publishf("SYSTEM,event=remote_restart");
    (void)capture_store_record_system("remote_restart");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_sendstr(request, "{\"ok\":true,\"restarting\":true}");
    xTaskCreate(delayed_restart_task, "api_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t web_service_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), "web", "http server");

    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = logs_handler},
        {.uri = "/api/marker", .method = HTTP_POST, .handler = marker_handler},
        {.uri = "/api/capture/status", .method = HTTP_GET, .handler = capture_status_handler},
        {.uri = "/api/capture/export", .method = HTTP_GET, .handler = capture_export_handler},
        {.uri = "/api/ac-state", .method = HTTP_GET, .handler = ac_state_handler},
        {.uri = "/api/control", .method = HTTP_POST, .handler = control_handler},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_handler},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_handler},
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &handlers[i]), "web", "URI");
    }
    telemetry_publishf("WEB,event=started,url=http://klima-wifi.local/");
    return ESP_OK;
}
