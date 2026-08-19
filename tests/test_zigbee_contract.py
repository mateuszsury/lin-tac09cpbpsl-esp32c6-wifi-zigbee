from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVICE = ROOT / "optional_components" / "zigbee_service" / "zigbee_service.c"
HEADER = ROOT / "optional_components" / "zigbee_service" / "include" / "zigbee_service.h"
CONVERTER = ROOT / "zigbee2mqtt" / "klima_wifi_converter.js"


def test_service_contract_and_identity():
    source = SERVICE.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    assert "esp_err_t zigbee_service_start(void);" in header
    assert "EZB_NWK_DEVICE_TYPE_END_DEVICE" in source
    assert '"zb_storage"' in source
    assert "ESP_ZIGBEE_RADIO_MODE_NATIVE" in source
    assert "Klima WiFi contributors" in source
    assert "Klima WiFi AC Bridge" in source
    assert "KLIMA_CLUSTER_ID 0xFC10U" in source
    assert "KLIMA_MANUFACTURER_CODE 0x131BU" in source
    assert "esp_app_get_description()" in source
    assert "static uint8_t s_sw_build_id[33]" in source


def test_all_firmware_controls_queue_through_bridge():
    source = SERVICE.read_text(encoding="utf-8")
    assert source.count("bridge_service_queue_control") == 1
    assert "ezb_zcl_set_attr_value" in source  # reporting only; callback does not mutate control attrs
    assert "EZB_ZCL_STATUS_READ_ONLY" in source
    assert "EZB_ZCL_THERMOSTAT_SYSTEM_MODE_SLEEP" not in source


def test_converter_is_read_reported_and_fail_closed():
    source = CONVERTER.read_text(encoding="utf-8")
    assert "control_available" in source
    assert "requireControl(meta)" in source
    assert "return {};" in source
    assert "manufacturerCode: MANUFACTURER_CODE" in source
    assert "0xFC10" in source
    assert "withSystemMode(['off', 'cool', 'fan_only', 'dry'])" in source
    assert "withFanMode(['low', 'high'])" in source
    assert "swing" not in source.lower()
    assert "ATTR_QUIET" in source and "ATTR_UNITS_FAHRENHEIT" in source
    assert "ATTR_TIMER" in source
    assert "exposes.binary('quiet', ea.ALL" in source
    assert "exposes.binary('timer', ea.STATE" in source
    assert "'quiet', 'units_fahrenheit', 'timer'" not in source
    assert "command_status" in source
    assert "command_timeouts" in source
