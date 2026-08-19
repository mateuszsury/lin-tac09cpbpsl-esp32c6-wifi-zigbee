from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main" / "mqtt_service.c").read_text(encoding="utf-8")
HEADER = (ROOT / "main" / "mqtt_service.h").read_text(encoding="utf-8")
SECRETS = (ROOT / "main" / "klima_secrets.h").read_text(encoding="utf-8")
EXAMPLE = (ROOT / "main" / "klima_secrets.example.h").read_text(encoding="utf-8")


def test_service_public_contract_and_guard():
    assert "esp_err_t mqtt_service_start(void)" in HEADER
    assert "void mqtt_service_publish_state(void)" in HEADER
    assert "KLIMA_HA_MQTT_ACTIVE" in SOURCE
    assert "#if KLIMA_HA_MQTT_ACTIVE" in SOURCE


def test_mqtt_transport_and_stable_topics():
    assert '#include "mqtt_client.h"' in SOURCE
    assert '#define MQTT_BASE_TOPIC "klima"' in SOURCE
    assert '"klima_%02x%02x%02x%02x%02x%02x"' in SOURCE
    assert ".session.last_will.topic" in SOURCE
    assert '.session.last_will.msg = "offline"' in SOURCE
    assert ".session.last_will.retain = true" in SOURCE
    assert 'publish_text(s_mqtt.availability_topic, "online", 1, 1)' in SOURCE


def test_passive_read_only_and_mitm_control_mapping():
    assert "bridge_service_mitm_active()" in SOURCE
    assert 'publish_command_error("passive_read_only")' in SOURCE
    assert "bridge_service_queue_control" in SOURCE
    assert '"off"' in SOURCE and '"cool"' in SOURCE
    assert '"fan_only"' in SOURCE and '"dry"' in SOURCE
    assert 'value < 18 || value > 32' in SOURCE
    assert '"low"' in SOURCE and '"high"' in SOURCE
    assert '"power_command_topic"' in SOURCE
    assert '"temperature_unit", "C"' in SOURCE
    assert '"current_temperature_topic"' in SOURCE
    assert '"current_temperature_template"' in SOURCE
    assert 'cJSON_AddNullToObject(root, "setpoint_c")' in SOURCE
    assert 'cJSON_AddNullToObject(root, "main_display_temperature_c")' in SOURCE
    assert 'cJSON_AddNullToObject(root, "panel_age_ms")' in SOURCE
    assert '"quiet"' in SOURCE and "AC_CONTROL_QUIET" in SOURCE
    assert '"units_fahrenheit"' in SOURCE and "AC_CONTROL_UNITS_FAHRENHEIT" in SOURCE
    assert 'cJSON_AddBoolToObject(root, "timer"' in SOURCE
    assert 'remove_discovery("switch", "timer")' in SOURCE
    assert "timer_set_topic" not in SOURCE
    assert '"optimistic", false' in SOURCE


def test_safe_command_and_ha_birth_contract():
    assert "event->retain" in SOURCE
    assert '"retained_command_rejected"' in SOURCE
    assert '"payload_must_not_be_fragmented"' in SOURCE
    assert '"command/accepted"' in SOURCE and '"command/error"' in SOURCE
    assert 'publish_json_text(topic, root, 1, 0)' in SOURCE
    assert '"command_status"' in SOURCE
    assert '"command_timeouts"' in SOURCE
    assert '"homeassistant/status"' in SOURCE
    assert 'memcmp(data, "online", 6U)' in SOURCE
    assert '"homeassistant/%s/%s/%s/config"' in SOURCE
    assert '"availability_topic"' in SOURCE
    assert '"control_availability"' in SOURCE
    assert '"availability_mode", "all"' in SOURCE
    assert "state.panel_valid && !state.panel_emulated" in SOURCE
    assert '"origin"' in SOURCE and '"device"' in SOURCE
    assert 'MQTT_DISCOVERY_STAGGER_MS' in SOURCE
    assert 'MQTT,event=connected' in SOURCE
    assert 'MQTT,event=disconnected' in SOURCE
    assert 'wifi_service_get_status(&wifi)' in SOURCE
    assert 'if (wifi.sta_connected' in SOURCE
    assert 'esp_mqtt_client_start(s_mqtt.client)' in SOURCE


def test_temperature_parser_accepts_ha_integral_decimal_but_stays_integer_only():
    assert "Home Assistant serializes climate temperatures as decimal JSON numbers" in SOURCE
    assert "while (*end == '0')" in SOURCE
    assert "if (!is_space_only(end))" in SOURCE


def test_tracked_headers_only_contain_safe_mqtt_defaults():
    for text in (SECRETS, EXAMPLE):
        assert "KLIMA_MQTT_BROKER_URI" in text
        assert "KLIMA_MQTT_USERNAME" in text
        assert "KLIMA_MQTT_PASSWORD" in text
        assert "KLIMA_MQTT_DEVICE_ID" in text
        assert "mqtt://192." not in text
        assert "password123" not in text.lower()
