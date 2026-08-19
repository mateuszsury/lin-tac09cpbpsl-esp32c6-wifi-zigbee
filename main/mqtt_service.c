#include "mqtt_service.h"

/* The source is kept in the main component so the same PASSIVE and MITM
 * images share one HA contract.  A build which intentionally omits the MQTT
 * component can set KLIMA_HA_MQTT_ACTIVE=0 and gets the harmless stubs below. */
#ifndef KLIMA_HA_MQTT_ACTIVE
#define KLIMA_HA_MQTT_ACTIVE 1
#endif

#if KLIMA_HA_MQTT_ACTIVE

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ac_protocol.h"
#include "bridge_service.h"
#include "capture_store.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "klima_secrets.h"
#include "mqtt_client.h"
#include "telemetry.h"
#include "wifi_service.h"

static const char *TAG = "klima_mqtt";

#define MQTT_BASE_TOPIC "klima"
#define MQTT_STATE_PERIOD_MS 5000U
#define MQTT_DISCOVERY_STAGGER_MS 150U
#define MQTT_WORKER_PERIOD_MS 250U
#define MQTT_MAX_COMMAND_PAYLOAD 768U
#ifndef PROJECT_VER
#define PROJECT_VER "unknown"
#endif
#define MQTT_FIRMWARE_VERSION PROJECT_VER

typedef struct {
    const char *component;
    const char *object_id;
    const char *name;
    const char *value_template;
    const char *device_class;
    const char *unit;
} diagnostic_entity_t;

static const diagnostic_entity_t k_diagnostics[] = {
    {"binary_sensor", "panel_link", "Panel link", "{{ 'ON' if value_json.panel_valid else 'OFF' }}", "connectivity", NULL},
    {"binary_sensor", "main_link", "Main controller link", "{{ 'ON' if value_json.main_valid else 'OFF' }}", "connectivity", NULL},
    {"binary_sensor", "mitm_active", "MITM active", "{{ 'ON' if value_json.mitm_active else 'OFF' }}", NULL, NULL},
    {"binary_sensor", "command_pending", "Command pending", "{{ 'ON' if value_json.command_pending else 'OFF' }}", NULL, NULL},
    {"sensor", "main_age_ms", "Main frame age", "{{ value_json.main_age_ms }}", "duration", "ms"},
    {"sensor", "panel_age_ms", "Panel frame age", "{{ value_json.panel_age_ms }}", "duration", "ms"},
    {"sensor", "checksum_errors", "Checksum errors", "{{ value_json.checksum_errors }}", NULL, NULL},
    {"sensor", "framing_errors", "Framing errors", "{{ value_json.framing_errors }}", NULL, NULL},
    {"sensor", "injected_frames", "Injected frames", "{{ value_json.injected_frames }}", NULL, NULL},
    {"sensor", "profile", "Bridge profile", "{{ value_json.profile }}", NULL, NULL},
    {"sensor", "feature_flags", "Raw feature flags", "{{ value_json.feature_flags }}", NULL, NULL},
    {"sensor", "main_display_temperature", "Main display temperature", "{{ value_json.main_display_temperature_c }}", "temperature", "°C"},
    {"sensor", "command_sequence", "Command sequence", "{{ value_json.command_sequence }}", NULL, NULL},
    {"sensor", "command_status", "Last command status", "{{ value_json.command_status }}", NULL, NULL},
    {"sensor", "command_timeouts", "Command timeouts", "{{ value_json.command_timeouts }}", NULL, NULL},
    {"sensor", "capture_records", "Stored capture records", "{{ value_json.capture_records }}", NULL, NULL},
    {"sensor", "capture_dropped", "Dropped capture records", "{{ value_json.capture_dropped }}", NULL, NULL},
    {"sensor", "capture_write_errors", "Capture write errors", "{{ value_json.capture_write_errors }}", NULL, NULL},
};

typedef struct {
    esp_mqtt_client_handle_t client;
    char device_id[32];
    char base_topic[64];
    char state_topic[96];
    char availability_topic[96];
    char control_availability_topic[96];
    char climate_set_topic[96];
    char mode_set_topic[96];
    char temperature_set_topic[96];
    char fan_set_topic[96];
    char power_set_topic[96];
    char quiet_set_topic[96];
    char units_set_topic[96];
    volatile bool connected;
    volatile bool started;
    volatile bool discovery_pending;
    volatile bool state_pending;
    TaskHandle_t worker;
} mqtt_context_t;

static mqtt_context_t s_mqtt;

static void topic_from_suffix(char *out, size_t out_len, const char *suffix)
{
    (void)snprintf(out, out_len, "%s/%s/%s", MQTT_BASE_TOPIC, s_mqtt.device_id, suffix);
}

static bool is_space_only(const char *text)
{
    if (!text) {
        return false;
    }
    while (*text != '\0') {
        if (!isspace((unsigned char)*text)) {
            return false;
        }
        ++text;
    }
    return true;
}

static void publish_text(const char *topic, const char *payload, int qos, int retain)
{
    if (s_mqtt.client == NULL || !s_mqtt.connected || topic == NULL || payload == NULL) {
        return;
    }
    (void)esp_mqtt_client_publish(s_mqtt.client, topic, payload, 0, qos, retain);
}

static void publish_json_text(const char *topic, cJSON *root, int qos, int retain)
{
    if (root == NULL) {
        return;
    }
    char *payload = cJSON_PrintUnformatted(root);
    if (payload != NULL) {
        publish_text(topic, payload, qos, retain);
        free(payload);
    }
    cJSON_Delete(root);
}

static cJSON *new_device_metadata(void)
{
    cJSON *device = cJSON_CreateObject();
    if (device == NULL) {
        return NULL;
    }
    cJSON *identifiers = cJSON_CreateArray();
    if (identifiers == NULL) {
        cJSON_Delete(device);
        return NULL;
    }
    cJSON_AddItemToArray(identifiers, cJSON_CreateString(s_mqtt.device_id));
    cJSON_AddItemToObject(device, "identifiers", identifiers);
    cJSON_AddStringToObject(device, "name", "Klima WiFi");
    cJSON_AddStringToObject(device, "manufacturer", "Klima WiFi");
    cJSON_AddStringToObject(device, "model", "TD-YD-PSL bridge");
    cJSON_AddStringToObject(device, "sw_version", MQTT_FIRMWARE_VERSION);
    return device;
}

static cJSON *new_discovery_root(const char *name, const char *object_id)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    char unique_id[80];
    (void)snprintf(unique_id, sizeof(unique_id), "%s_%s", s_mqtt.device_id, object_id);
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "unique_id", unique_id);
    cJSON_AddStringToObject(root, "availability_topic", s_mqtt.availability_topic);
    cJSON_AddStringToObject(root, "payload_available", "online");
    cJSON_AddStringToObject(root, "payload_not_available", "offline");
    cJSON_AddItemToObject(root, "device", new_device_metadata());
    cJSON *origin = cJSON_CreateObject();
    if (origin != NULL) {
        cJSON_AddStringToObject(origin, "name", "Klima WiFi MQTT");
        cJSON_AddStringToObject(origin, "sw_version", MQTT_FIRMWARE_VERSION);
        cJSON_AddItemToObject(root, "origin", origin);
    }
    return root;
}

static void publish_discovery_root(const char *component, const char *object_id, cJSON *root)
{
    char topic[160];
    (void)snprintf(topic, sizeof(topic), "homeassistant/%s/%s/%s/config", component, s_mqtt.device_id, object_id);
    publish_json_text(topic, root, 1, 1);
}

static void remove_discovery(const char *component, const char *object_id)
{
    char topic[160];
    (void)snprintf(
        topic,
        sizeof(topic),
        "homeassistant/%s/%s/%s/config",
        component,
        s_mqtt.device_id,
        object_id);
    publish_text(topic, "", 1, 1);
}

static cJSON *new_availability_condition(const char *topic)
{
    cJSON *condition = cJSON_CreateObject();
    if (condition == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(condition, "topic", topic);
    cJSON_AddStringToObject(condition, "payload_available", "online");
    cJSON_AddStringToObject(condition, "payload_not_available", "offline");
    return condition;
}

static bool require_control_availability(cJSON *root)
{
    cJSON *availability = cJSON_CreateArray();
    if (availability == NULL) {
        return false;
    }
    cJSON *device_condition = new_availability_condition(s_mqtt.availability_topic);
    cJSON *control_condition =
        new_availability_condition(s_mqtt.control_availability_topic);
    if (device_condition == NULL || control_condition == NULL) {
        cJSON_Delete(device_condition);
        cJSON_Delete(control_condition);
        cJSON_Delete(availability);
        return false;
    }
    cJSON_AddItemToArray(availability, device_condition);
    cJSON_AddItemToArray(availability, control_condition);
    cJSON_DeleteItemFromObjectCaseSensitive(root, "availability_topic");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "payload_available");
    cJSON_DeleteItemFromObjectCaseSensitive(root, "payload_not_available");
    cJSON_AddItemToObject(root, "availability", availability);
    cJSON_AddStringToObject(root, "availability_mode", "all");
    return true;
}

static const char *ha_mode_name(const bridge_ac_state_t *state)
{
    if (!state->power) {
        return "off";
    }
    if (state->mode == AC_MODE_FAN) {
        return "fan_only";
    }
    return ac_protocol_mode_name(state->mode);
}

static const char *ha_fan_name(const bridge_ac_state_t *state)
{
    if (state->fan == AC_FAN_LOW) {
        return "low";
    }
    if (state->fan == AC_FAN_HIGH) {
        return "high";
    }
    return "unknown";
}

static cJSON *build_state_json(void)
{
    bridge_ac_state_t state = {0};
    capture_store_info_t capture = {0};
    bridge_service_get_ac_state(&state);
    capture_store_get_info(&capture);
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "profile", bridge_service_profile_name());
    cJSON_AddBoolToObject(root, "main_valid", state.main_valid);
    cJSON_AddBoolToObject(root, "panel_valid", state.panel_valid);
    if (state.main_age_ms >= 0) {
        cJSON_AddNumberToObject(root, "main_age_ms", (double)state.main_age_ms);
    } else {
        cJSON_AddNullToObject(root, "main_age_ms");
    }
    if (state.panel_age_ms >= 0) {
        cJSON_AddNumberToObject(root, "panel_age_ms", (double)state.panel_age_ms);
    } else {
        cJSON_AddNullToObject(root, "panel_age_ms");
    }
    cJSON_AddBoolToObject(root, "power", state.power);
    cJSON_AddNumberToObject(root, "feature_flags", state.feature_flags);
    cJSON_AddBoolToObject(root, "quiet", (state.feature_flags & AC_FEATURE_QUIET) != 0U);
    cJSON_AddBoolToObject(root, "units_fahrenheit",
                          (state.feature_flags & AC_FEATURE_UNITS_FAHRENHEIT) != 0U);
    cJSON_AddBoolToObject(root, "timer", (state.feature_flags & AC_FEATURE_TIMER) != 0U);
    cJSON_AddStringToObject(root, "mode", ha_mode_name(&state));
    cJSON_AddStringToObject(root, "fan", ha_fan_name(&state));
    if (state.setpoint_c >= 18U && state.setpoint_c <= 32U) {
        cJSON_AddNumberToObject(root, "setpoint_c", state.setpoint_c);
    } else {
        cJSON_AddNullToObject(root, "setpoint_c");
    }
    if (state.main_display_temperature_c > 0U &&
        state.main_display_temperature_c <= 60U) {
        cJSON_AddNumberToObject(
            root, "main_display_temperature_c", state.main_display_temperature_c);
    } else {
        cJSON_AddNullToObject(root, "main_display_temperature_c");
    }
    cJSON_AddNumberToObject(root, "main_display_temperature_encoded", state.main_display_temperature_encoded);
    cJSON_AddNumberToObject(root, "mode_fan_code", state.mode_fan_code);
    cJSON_AddNumberToObject(root, "operation_code", state.operation_code);
    cJSON_AddNumberToObject(root, "sensor_1_raw", state.sensor_1_raw);
    cJSON_AddNumberToObject(root, "sensor_2_raw", state.sensor_2_raw);
    cJSON_AddBoolToObject(root, "panel_event", state.panel_event);
    cJSON_AddBoolToObject(root, "panel_emulated", state.panel_emulated);
    cJSON_AddBoolToObject(root, "mitm_active", state.mitm_active);
    cJSON_AddBoolToObject(root, "override_active", state.override_active);
    cJSON_AddBoolToObject(root, "command_pending", state.command_pending);
    cJSON_AddNumberToObject(root, "command_sequence", state.command_sequence);
    cJSON_AddStringToObject(
        root, "command_status", bridge_service_command_status_name(state.command_status));
    cJSON_AddNumberToObject(root, "command_timeouts", state.command_timeouts);
    cJSON_AddNumberToObject(root, "main_frames", state.main_frames);
    cJSON_AddNumberToObject(root, "panel_frames", state.panel_frames);
    cJSON_AddNumberToObject(root, "main_rx_bytes", state.main_rx_bytes);
    cJSON_AddNumberToObject(root, "panel_rx_bytes", state.panel_rx_bytes);
    cJSON_AddNumberToObject(root, "main_rx_level", state.main_rx_level);
    cJSON_AddNumberToObject(root, "panel_rx_level", state.panel_rx_level);
    cJSON_AddNumberToObject(root, "alternate_panel_rx_level", state.alternate_panel_rx_level);
    cJSON_AddNumberToObject(root, "panel_candidate_gpio5_level", state.panel_candidate_gpio5_level);
    cJSON_AddNumberToObject(root, "panel_tx_pad_level", state.panel_tx_pad_level);
    cJSON_AddNumberToObject(root, "main_tx_pad_level", state.main_tx_pad_level);
    cJSON_AddNumberToObject(root, "mirror_main_edges", state.mirror_main_edges);
    cJSON_AddNumberToObject(root, "alternate_panel_edges", state.alternate_panel_edges);
    cJSON_AddNumberToObject(root, "panel_candidate_gpio5_edges", state.panel_candidate_gpio5_edges);
    cJSON_AddNumberToObject(root, "panel_tx_pad_edges", state.panel_tx_pad_edges);
    cJSON_AddNumberToObject(root, "main_tx_pad_edges", state.main_tx_pad_edges);
    cJSON_AddNumberToObject(root, "checksum_errors", state.checksum_errors);
    cJSON_AddNumberToObject(root, "framing_errors", state.framing_errors);
    cJSON_AddNumberToObject(root, "injected_frames", state.injected_frames);
    cJSON_AddNumberToObject(root, "capture_records", capture.valid_records);
    cJSON_AddNumberToObject(root, "capture_capacity", capture.capacity_records);
    cJSON_AddNumberToObject(root, "capture_latest_sequence", capture.latest_sequence);
    cJSON_AddNumberToObject(root, "capture_dropped", capture.dropped_records);
    cJSON_AddNumberToObject(root, "capture_write_errors", capture.write_errors);
    return root;
}

static void publish_state_now(void)
{
    bridge_ac_state_t state = {0};
    bridge_service_get_ac_state(&state);
    const bool control_available = state.mitm_active && state.main_valid &&
        state.panel_valid && !state.panel_emulated;
    publish_text(
        s_mqtt.control_availability_topic,
        control_available ? "online" : "offline",
        1,
        1);
    cJSON *root = build_state_json();
    if (root != NULL) {
        publish_json_text(s_mqtt.state_topic, root, 1, 1);
    }
}

static void publish_climate_discovery(void)
{
    if (!bridge_service_mitm_active()) {
        return;
    }
    cJSON *root = new_discovery_root("Klima", "climate");
    if (root == NULL) {
        return;
    }
    if (!require_control_availability(root)) {
        cJSON_Delete(root);
        return;
    }
    cJSON_AddStringToObject(root, "state_topic", s_mqtt.state_topic);
    cJSON_AddStringToObject(root, "mode_state_topic", s_mqtt.state_topic);
    cJSON_AddStringToObject(root, "temperature_state_topic", s_mqtt.state_topic);
    cJSON_AddStringToObject(root, "current_temperature_topic", s_mqtt.state_topic);
    cJSON_AddStringToObject(root, "fan_mode_state_topic", s_mqtt.state_topic);
    cJSON_AddStringToObject(root, "mode_state_template", "{{ value_json.mode }}");
    cJSON_AddStringToObject(root, "temperature_state_template", "{{ value_json.setpoint_c }}");
    cJSON_AddStringToObject(
        root,
        "current_temperature_template",
        "{{ value_json.main_display_temperature_c }}");
    cJSON_AddStringToObject(root, "fan_mode_state_template", "{{ value_json.fan }}");
    cJSON_AddStringToObject(root, "mode_command_topic", s_mqtt.mode_set_topic);
    cJSON_AddStringToObject(root, "temperature_command_topic", s_mqtt.temperature_set_topic);
    cJSON_AddStringToObject(root, "fan_mode_command_topic", s_mqtt.fan_set_topic);
    cJSON_AddStringToObject(root, "power_command_topic", s_mqtt.power_set_topic);
    cJSON_AddStringToObject(root, "payload_on", "ON");
    cJSON_AddStringToObject(root, "payload_off", "OFF");
    cJSON_AddStringToObject(root, "temperature_command_template", "{{ value }}");
    cJSON_AddStringToObject(root, "temperature_unit", "C");
    cJSON_AddNumberToObject(root, "min_temp", 18);
    cJSON_AddNumberToObject(root, "max_temp", 32);
    cJSON_AddNumberToObject(root, "temp_step", 1);
    cJSON *modes = cJSON_CreateArray();
    cJSON_AddItemToArray(modes, cJSON_CreateString("off"));
    cJSON_AddItemToArray(modes, cJSON_CreateString("cool"));
    cJSON_AddItemToArray(modes, cJSON_CreateString("fan_only"));
    cJSON_AddItemToArray(modes, cJSON_CreateString("dry"));
    cJSON_AddItemToObject(root, "modes", modes);
    cJSON *fans = cJSON_CreateArray();
    cJSON_AddItemToArray(fans, cJSON_CreateString("low"));
    cJSON_AddItemToArray(fans, cJSON_CreateString("high"));
    cJSON_AddItemToObject(root, "fan_modes", fans);
    cJSON_AddNumberToObject(root, "precision", 1.0);
    publish_discovery_root("climate", "climate", root);
}

static void publish_diagnostic_discovery(const diagnostic_entity_t *entity)
{
    cJSON *root = new_discovery_root(entity->name, entity->object_id);
    if (root == NULL) {
        return;
    }
    cJSON_AddStringToObject(root, "state_topic", s_mqtt.state_topic);
    cJSON_AddStringToObject(root, "value_template", entity->value_template);
    cJSON_AddStringToObject(root, "entity_category", "diagnostic");
    if (strcmp(entity->component, "binary_sensor") == 0) {
        cJSON_AddStringToObject(root, "payload_on", "ON");
        cJSON_AddStringToObject(root, "payload_off", "OFF");
    }
    if (entity->device_class != NULL) {
        cJSON_AddStringToObject(root, "device_class", entity->device_class);
    }
    if (entity->unit != NULL) {
        cJSON_AddStringToObject(root, "unit_of_measurement", entity->unit);
    }
    publish_discovery_root(entity->component, entity->object_id, root);
}

static void publish_feature_discovery(const char *object_id, const char *name,
                                      const char *value_template, const char *command_topic)
{
    if (!bridge_service_mitm_active()) {
        return;
    }
    cJSON *root = new_discovery_root(name, object_id);
    if (root == NULL) {
        return;
    }
    if (!require_control_availability(root)) {
        cJSON_Delete(root);
        return;
    }
    cJSON_AddStringToObject(root, "state_topic", s_mqtt.state_topic);
    cJSON_AddStringToObject(root, "value_template", value_template);
    cJSON_AddStringToObject(root, "command_topic", command_topic);
    cJSON_AddStringToObject(root, "payload_on", "ON");
    cJSON_AddStringToObject(root, "payload_off", "OFF");
    cJSON_AddBoolToObject(root, "optimistic", false);
    publish_discovery_root("switch", object_id, root);
}

static void publish_discovery(void)
{
    publish_climate_discovery();
    vTaskDelay(pdMS_TO_TICKS(MQTT_DISCOVERY_STAGGER_MS));
    publish_feature_discovery("quiet", "Quiet", "{{ 'ON' if value_json.quiet else 'OFF' }}",
                              s_mqtt.quiet_set_topic);
    vTaskDelay(pdMS_TO_TICKS(MQTT_DISCOVERY_STAGGER_MS));
    publish_feature_discovery("units_fahrenheit", "Jednostki Fahrenheit",
                              "{{ 'ON' if value_json.units_fahrenheit else 'OFF' }}",
                              s_mqtt.units_set_topic);
    vTaskDelay(pdMS_TO_TICKS(MQTT_DISCOVERY_STAGGER_MS));
    /* Delete discovery retained by older builds. The active bit remains in
     * state JSON for diagnostics, but a valid timer command also needs an
     * undecoded duration field. */
    remove_discovery("switch", "timer");
    vTaskDelay(pdMS_TO_TICKS(MQTT_DISCOVERY_STAGGER_MS));
    for (size_t i = 0; i < sizeof(k_diagnostics) / sizeof(k_diagnostics[0]); ++i) {
        if (!s_mqtt.connected) {
            return;
        }
        publish_diagnostic_discovery(&k_diagnostics[i]);
        vTaskDelay(pdMS_TO_TICKS(MQTT_DISCOVERY_STAGGER_MS));
    }
}

static void publish_command_error(const char *error)
{
    char topic[112];
    topic_from_suffix(topic, sizeof(topic), "command/error");
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddBoolToObject(root, "accepted", false);
    cJSON_AddStringToObject(root, "error", error != NULL ? error : "invalid_command");
    cJSON_AddStringToObject(root, "profile", bridge_service_profile_name());
    publish_json_text(topic, root, 1, 0);
}

static void publish_command_accepted(uint32_t sequence, const char *command)
{
    char topic[112];
    topic_from_suffix(topic, sizeof(topic), "command/accepted");
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddBoolToObject(root, "accepted", true);
    cJSON_AddNumberToObject(root, "sequence", sequence);
    cJSON_AddStringToObject(root, "command", command != NULL ? command : "climate");
    publish_json_text(topic, root, 1, 0);
}

static bool parse_complete_json(const char *data, size_t data_len, cJSON **root_out)
{
    if (root_out == NULL || data == NULL || data_len == 0U || data_len > MQTT_MAX_COMMAND_PAYLOAD) {
        return false;
    }
    char *copy = (char *)calloc(data_len + 1U, 1U);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, data, data_len);
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(copy, data_len, &end, false);
    bool valid = root != NULL && cJSON_IsObject(root) && end != NULL && is_space_only(end);
    free(copy);
    if (!valid) {
        cJSON_Delete(root);
        return false;
    }
    *root_out = root;
    return true;
}

static bool parse_mode_name(const char *value, ac_mode_t *mode, bool *power)
{
    if (!value || !mode || !power) {
        return false;
    }
    if (strcmp(value, "off") == 0) {
        *mode = AC_MODE_UNKNOWN;
        *power = false;
        return true;
    }
    if (strcmp(value, "cool") == 0) {
        *mode = AC_MODE_COOL;
    } else if (strcmp(value, "fan_only") == 0) {
        *mode = AC_MODE_FAN;
    } else if (strcmp(value, "dry") == 0) {
        *mode = AC_MODE_DRY;
    } else {
        return false;
    }
    *power = true;
    return true;
}

static bool parse_fan_name(const char *value, ac_fan_t *fan)
{
    if (!value || !fan) {
        return false;
    }
    if (strcmp(value, "low") == 0) {
        *fan = AC_FAN_LOW;
        return true;
    }
    if (strcmp(value, "high") == 0) {
        *fan = AC_FAN_HIGH;
        return true;
    }
    return false;
}

static bool parse_integer_payload(const char *data, size_t data_len, int *value)
{
    if (!data || !value || data_len == 0U || data_len > 32U) {
        return false;
    }
    char buffer[33] = {0};
    memcpy(buffer, data, data_len);
    char *start = buffer;
    while (isspace((unsigned char)*start)) {
        ++start;
    }
    char *end = NULL;
    long parsed = strtol(start, &end, 10);
    if (end == start || parsed < INT_MIN || parsed > INT_MAX) {
        return false;
    }
    /* Home Assistant serializes climate temperatures as decimal JSON numbers
     * (for example "19.0") even when temp_step is exactly one degree. Accept
     * only a fractional part made entirely of zeroes so the AC's integer
     * 18..32 C contract remains strict. */
    if (*end == '.') {
        ++end;
        if (*end == '\0' || !isdigit((unsigned char)*end)) {
            return false;
        }
        while (*end == '0') {
            ++end;
        }
    }
    if (!is_space_only(end)) {
        return false;
    }
    *value = (int)parsed;
    return true;
}

static bool parse_string_payload(const char *data, size_t data_len, char *out, size_t out_len)
{
    if (!data || !out || out_len == 0U || data_len == 0U || data_len > MQTT_MAX_COMMAND_PAYLOAD) {
        return false;
    }
    size_t first = 0U;
    while (first < data_len && isspace((unsigned char)data[first])) {
        ++first;
    }
    size_t last = data_len;
    while (last > first && isspace((unsigned char)data[last - 1U])) {
        --last;
    }
    if (last <= first || last - first >= out_len) {
        return false;
    }
    if (data[first] == '"') {
        cJSON *root = NULL;
        if (!parse_complete_json(data + first, last - first, &root) || !cJSON_IsString(root)) {
            cJSON_Delete(root);
            return false;
        }
        (void)snprintf(out, out_len, "%s", root->valuestring);
        cJSON_Delete(root);
        return true;
    }
    memcpy(out, data + first, last - first);
    out[last - first] = '\0';
    return true;
}

static bool queue_request(const ac_control_request_t *request, const char *command)
{
    if (!bridge_service_mitm_active()) {
        publish_command_error("passive_read_only");
        return false;
    }
    uint32_t sequence = 0U;
    const esp_err_t result = bridge_service_queue_control(request, &sequence);
    if (result != ESP_OK) {
        publish_command_error(esp_err_to_name(result));
        return false;
    }
    publish_command_accepted(sequence, command);
    s_mqtt.state_pending = true;
    return true;
}

static bool handle_full_climate_command(const char *data, size_t data_len)
{
    cJSON *root = NULL;
    if (!parse_complete_json(data, data_len, &root)) {
        publish_command_error("payload_must_be_complete_json_object");
        return false;
    }
    const cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(root, "mode");
    const cJSON *temp_item = cJSON_GetObjectItemCaseSensitive(root, "target_temp");
    const cJSON *fan_item = cJSON_GetObjectItemCaseSensitive(root, "fan");
    const cJSON *power_item = cJSON_GetObjectItemCaseSensitive(root, "power");
    bool valid = cJSON_IsString(mode_item) && cJSON_IsNumber(temp_item) &&
        cJSON_IsString(fan_item) && cJSON_IsBool(power_item) &&
        temp_item->valuedouble >= 18.0 && temp_item->valuedouble <= 32.0 &&
        temp_item->valuedouble == (double)temp_item->valueint;
    ac_mode_t mode = AC_MODE_UNKNOWN;
    ac_fan_t fan = AC_FAN_UNKNOWN;
    bool power = false;
    if (valid) {
        valid = parse_mode_name(mode_item->valuestring, &mode, &power) &&
            parse_fan_name(fan_item->valuestring, &fan) &&
            (cJSON_IsTrue(power_item) == power);
    }
    if (!valid) {
        cJSON_Delete(root);
        publish_command_error("payload_requires_mode_target_temp_fan_power");
        return false;
    }
    ac_control_request_t request = {
        .mask = AC_CONTROL_POWER | AC_CONTROL_SETPOINT | AC_CONTROL_FAN,
        .power = power,
        .setpoint_c = (uint8_t)temp_item->valueint,
        .fan = fan,
    };
    if (mode != AC_MODE_UNKNOWN) {
        request.mask |= AC_CONTROL_MODE;
        request.mode = mode;
    }
    cJSON_Delete(root);
    return queue_request(&request, "climate");
}

static bool handle_mode_command(const char *data, size_t data_len)
{
    char mode_name[24];
    ac_mode_t mode = AC_MODE_UNKNOWN;
    bool power = false;
    if (!parse_string_payload(data, data_len, mode_name, sizeof(mode_name)) ||
        !parse_mode_name(mode_name, &mode, &power)) {
        publish_command_error("mode_must_be_off_cool_fan_only_or_dry");
        return false;
    }
    ac_control_request_t request = {
        .mask = AC_CONTROL_POWER,
        .power = power,
    };
    if (mode != AC_MODE_UNKNOWN) {
        request.mask |= AC_CONTROL_MODE;
        request.mode = mode;
    }
    return queue_request(&request, "mode");
}

static bool handle_temperature_command(const char *data, size_t data_len)
{
    int value = 0;
    if (!parse_integer_payload(data, data_len, &value) || value < 18 || value > 32) {
        publish_command_error("target_temp_must_be_an_integer_18_to_32");
        return false;
    }
    const ac_control_request_t request = {
        .mask = AC_CONTROL_SETPOINT,
        .setpoint_c = (uint8_t)value,
    };
    return queue_request(&request, "temperature");
}

static bool handle_fan_command(const char *data, size_t data_len)
{
    char fan_name[16];
    ac_fan_t fan = AC_FAN_UNKNOWN;
    if (!parse_string_payload(data, data_len, fan_name, sizeof(fan_name)) ||
        !parse_fan_name(fan_name, &fan)) {
        publish_command_error("fan_must_be_low_or_high");
        return false;
    }
    const ac_control_request_t request = {
        .mask = AC_CONTROL_FAN,
        .fan = fan,
    };
    return queue_request(&request, "fan");
}

static bool handle_power_command(const char *data, size_t data_len)
{
    char value[8];
    if (!parse_string_payload(data, data_len, value, sizeof(value)) ||
        (strcmp(value, "ON") != 0 && strcmp(value, "OFF") != 0)) {
        publish_command_error("power_must_be_ON_or_OFF");
        return false;
    }
    const ac_control_request_t request = {
        .mask = AC_CONTROL_POWER,
        .power = strcmp(value, "ON") == 0,
    };
    return queue_request(&request, "power");
}

static bool handle_feature_command(const char *data, size_t data_len,
                                   uint32_t mask, const char *command)
{
    char value[8];
    if (!parse_string_payload(data, data_len, value, sizeof(value)) ||
        (strcmp(value, "ON") != 0 && strcmp(value, "OFF") != 0)) {
        publish_command_error("feature_must_be_ON_or_OFF");
        return false;
    }
    const bool enabled = strcmp(value, "ON") == 0;
    ac_control_request_t request = {.mask = mask};
    if (mask == AC_CONTROL_QUIET) {
        request.quiet = enabled;
    } else if (mask == AC_CONTROL_UNITS_FAHRENHEIT) {
        request.units_fahrenheit = enabled;
    } else {
        publish_command_error("unsupported_feature");
        return false;
    }
    return queue_request(&request, command);
}

static void handle_command_topic(const char *topic, const char *data, size_t data_len)
{
    if (!bridge_service_mitm_active()) {
        publish_command_error("passive_read_only");
        return;
    }
    if (strcmp(topic, s_mqtt.climate_set_topic) == 0) {
        (void)handle_full_climate_command(data, data_len);
    } else if (strcmp(topic, s_mqtt.mode_set_topic) == 0) {
        (void)handle_mode_command(data, data_len);
    } else if (strcmp(topic, s_mqtt.temperature_set_topic) == 0) {
        (void)handle_temperature_command(data, data_len);
    } else if (strcmp(topic, s_mqtt.fan_set_topic) == 0) {
        (void)handle_fan_command(data, data_len);
    } else if (strcmp(topic, s_mqtt.power_set_topic) == 0) {
        (void)handle_power_command(data, data_len);
    } else if (strcmp(topic, s_mqtt.quiet_set_topic) == 0) {
        (void)handle_feature_command(data, data_len, AC_CONTROL_QUIET, "quiet");
    } else if (strcmp(topic, s_mqtt.units_set_topic) == 0) {
        (void)handle_feature_command(
            data, data_len, AC_CONTROL_UNITS_FAHRENHEIT, "units_fahrenheit");
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        s_mqtt.connected = true;
        telemetry_publishf("MQTT,event=connected,device_id=%s", s_mqtt.device_id);
        publish_text(s_mqtt.availability_topic, "online", 1, 1);
        /* HA birth is read-only coordination, while the climate command
         * subscriptions are deliberately restricted to the MITM image. */
        (void)esp_mqtt_client_subscribe(s_mqtt.client, "homeassistant/status", 0);
        if (bridge_service_mitm_active()) {
            (void)esp_mqtt_client_subscribe(s_mqtt.client, s_mqtt.climate_set_topic, 0);
            (void)esp_mqtt_client_subscribe(s_mqtt.client, s_mqtt.mode_set_topic, 0);
            (void)esp_mqtt_client_subscribe(s_mqtt.client, s_mqtt.temperature_set_topic, 0);
            (void)esp_mqtt_client_subscribe(s_mqtt.client, s_mqtt.fan_set_topic, 0);
            (void)esp_mqtt_client_subscribe(s_mqtt.client, s_mqtt.power_set_topic, 0);
            (void)esp_mqtt_client_subscribe(s_mqtt.client, s_mqtt.quiet_set_topic, 0);
            (void)esp_mqtt_client_subscribe(s_mqtt.client, s_mqtt.units_set_topic, 0);
        }
        s_mqtt.discovery_pending = true;
        s_mqtt.state_pending = true;
        return;
    }
    if (event_id == MQTT_EVENT_DISCONNECTED) {
        s_mqtt.connected = false;
        telemetry_publishf("MQTT,event=disconnected,device_id=%s", s_mqtt.device_id);
        return;
    }
    if (event_id == MQTT_EVENT_ERROR) {
        telemetry_publishf("MQTT,event=error,device_id=%s", s_mqtt.device_id);
        return;
    }
    if (event_id != MQTT_EVENT_DATA || event == NULL) {
        return;
    }
    if (event->topic_len <= 0 || event->data_len < 0 || event->total_data_len != event->data_len ||
        event->current_data_offset != 0) {
        publish_command_error("payload_must_not_be_fragmented");
        return;
    }
    char topic[160] = {0};
    char data[MQTT_MAX_COMMAND_PAYLOAD + 1U] = {0};
    size_t topic_len = (size_t)event->topic_len;
    size_t data_len = (size_t)event->data_len;
    if (topic_len >= sizeof(topic) || data_len > MQTT_MAX_COMMAND_PAYLOAD) {
        publish_command_error("payload_too_large");
        return;
    }
    memcpy(topic, event->topic, topic_len);
    memcpy(data, event->data, data_len);
    if (strcmp(topic, "homeassistant/status") == 0) {
        if (data_len == 6U && memcmp(data, "online", 6U) == 0) {
            s_mqtt.discovery_pending = true;
            s_mqtt.state_pending = true;
        }
        return;
    }
    /* Retained command payloads can replay stale actuator requests after a
     * reboot.  They are never acted upon, even in MITM mode. */
    if (event->retain) {
        ESP_LOGW(TAG, "ignoring retained MQTT command topic=%s", topic);
        publish_command_error("retained_command_rejected");
        return;
    }
    handle_command_topic(topic, data, data_len);
}

static void mqtt_worker(void *arg)
{
    (void)arg;
    TickType_t last_state = xTaskGetTickCount();
    TickType_t last_start_attempt = 0;
    for (;;) {
        if (!s_mqtt.started) {
            wifi_service_status_t wifi = {0};
            wifi_service_get_status(&wifi);
            const TickType_t now = xTaskGetTickCount();
            if (wifi.sta_connected &&
                (last_start_attempt == 0 ||
                 now - last_start_attempt >= pdMS_TO_TICKS(5000))) {
                last_start_attempt = now;
                const esp_err_t start_result = esp_mqtt_client_start(s_mqtt.client);
                if (start_result == ESP_OK) {
                    s_mqtt.started = true;
                    telemetry_publishf(
                        "MQTT,event=started,device_id=%s", s_mqtt.device_id);
                } else {
                    telemetry_publishf(
                        "MQTT,event=start_error,code=%s",
                        esp_err_to_name(start_result));
                }
            }
        }
        if (s_mqtt.connected) {
            if (s_mqtt.discovery_pending) {
                s_mqtt.discovery_pending = false;
                publish_discovery();
            }
            const TickType_t now = xTaskGetTickCount();
            if (s_mqtt.state_pending || now - last_state >= pdMS_TO_TICKS(MQTT_STATE_PERIOD_MS)) {
                publish_state_now();
                s_mqtt.state_pending = false;
                last_state = now;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(MQTT_WORKER_PERIOD_MS));
    }
}

static void choose_device_id(void)
{
    if (KLIMA_MQTT_DEVICE_ID[0] != '\0') {
        (void)snprintf(s_mqtt.device_id, sizeof(s_mqtt.device_id), "%s", KLIMA_MQTT_DEVICE_ID);
        return;
    }
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        (void)snprintf(s_mqtt.device_id, sizeof(s_mqtt.device_id), "klima_unknown");
        return;
    }
    /* The full STA MAC avoids collisions between otherwise identical bridge
     * builds and remains stable across firmware updates. */
    (void)snprintf(
        s_mqtt.device_id,
        sizeof(s_mqtt.device_id),
        "klima_%02x%02x%02x%02x%02x%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

esp_err_t mqtt_service_start(void)
{
    if (KLIMA_MQTT_BROKER_URI[0] == '\0') {
        ESP_LOGI(TAG, "MQTT disabled: KLIMA_MQTT_BROKER_URI is empty");
        return ESP_OK;
    }
    if (s_mqtt.client != NULL) {
        return ESP_OK;
    }
    memset(&s_mqtt, 0, sizeof(s_mqtt));
    choose_device_id();
    (void)snprintf(s_mqtt.base_topic, sizeof(s_mqtt.base_topic), "%s/%s", MQTT_BASE_TOPIC, s_mqtt.device_id);
    topic_from_suffix(s_mqtt.state_topic, sizeof(s_mqtt.state_topic), "state");
    topic_from_suffix(s_mqtt.availability_topic, sizeof(s_mqtt.availability_topic), "availability");
    topic_from_suffix(
        s_mqtt.control_availability_topic,
        sizeof(s_mqtt.control_availability_topic),
        "control_availability");
    topic_from_suffix(s_mqtt.climate_set_topic, sizeof(s_mqtt.climate_set_topic), "climate/set");
    topic_from_suffix(s_mqtt.mode_set_topic, sizeof(s_mqtt.mode_set_topic), "climate/mode/set");
    topic_from_suffix(s_mqtt.temperature_set_topic, sizeof(s_mqtt.temperature_set_topic), "climate/temperature/set");
    topic_from_suffix(s_mqtt.fan_set_topic, sizeof(s_mqtt.fan_set_topic), "climate/fan/set");
    topic_from_suffix(s_mqtt.power_set_topic, sizeof(s_mqtt.power_set_topic), "climate/power/set");
    topic_from_suffix(s_mqtt.quiet_set_topic, sizeof(s_mqtt.quiet_set_topic), "feature/quiet/set");
    topic_from_suffix(s_mqtt.units_set_topic, sizeof(s_mqtt.units_set_topic), "feature/units_fahrenheit/set");

    const esp_mqtt_client_config_t config = {
        .broker.address.uri = KLIMA_MQTT_BROKER_URI,
        .credentials.client_id = s_mqtt.device_id,
        .credentials.username = KLIMA_MQTT_USERNAME,
        .credentials.authentication.password = KLIMA_MQTT_PASSWORD,
        .session.last_will.topic = s_mqtt.availability_topic,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 30000,
        .network.timeout_ms = 15000,
        .task.stack_size = 8192,
        .buffer.size = 4096,
        .buffer.out_size = 4096,
        .outbox.limit = 8192,
    };
    s_mqtt.client = esp_mqtt_client_init(&config);
    if (s_mqtt.client == NULL) {
        memset(&s_mqtt, 0, sizeof(s_mqtt));
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_mqtt_client_register_event(s_mqtt.client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        (void)esp_mqtt_client_destroy(s_mqtt.client);
        memset(&s_mqtt, 0, sizeof(s_mqtt));
        return err;
    }
    if (xTaskCreate(mqtt_worker, "klima_mqtt", 6144, NULL, 4, &s_mqtt.worker) != pdPASS) {
        (void)esp_mqtt_client_destroy(s_mqtt.client);
        memset(&s_mqtt, 0, sizeof(s_mqtt));
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void mqtt_service_publish_state(void)
{
    if (s_mqtt.client != NULL && s_mqtt.connected) {
        s_mqtt.state_pending = true;
    }
}

#else

esp_err_t mqtt_service_start(void)
{
    return ESP_OK;
}

void mqtt_service_publish_state(void)
{
}

#endif
