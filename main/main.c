#include "bridge_service.h"
#include "capture_store.h"
#include "telemetry.h"
#ifndef KLIMA_CANONICAL_SNIFFER
#define KLIMA_CANONICAL_SNIFFER 0
#endif
#ifndef KLIMA_CANONICAL_BRIDGE
#define KLIMA_CANONICAL_BRIDGE 0
#endif
#ifndef KLIMA_CANONICAL_MITM_NTS
#define KLIMA_CANONICAL_MITM_NTS 0
#endif
#ifndef KLIMA_CANONICAL_PANEL_DIAG
#define KLIMA_CANONICAL_PANEL_DIAG 0
#endif
#ifndef KLIMA_CANONICAL_PANEL_BENCH
#define KLIMA_CANONICAL_PANEL_BENCH 0
#endif
#include "web_service.h"
#include "wifi_service.h"

#include "esp_err.h"
#include "esp_log.h"

#ifndef KLIMA_NTS0104_PREFLIGHT
#define KLIMA_NTS0104_PREFLIGHT 0
#endif
#if KLIMA_NTS0104_PREFLIGHT
#include "nts0104_preflight.h"
#endif

#if KLIMA_HA_MQTT_ACTIVE
#include "mqtt_service.h"
#endif
#if KLIMA_HA_ZIGBEE_ACTIVE
#include "esp_coexist.h"
/* The root build selects and links the optional Zigbee component.  Keeping
 * this narrow declaration here avoids making the always-present main
 * component pull Zigbee into MQTT-only firmware images. */
esp_err_t zigbee_service_start(void);
#endif

#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"

static const char *TAG = "klima_wifi";
#ifndef KLIMA_NTS0104_OUTPUTS_ENABLED
#define KLIMA_NTS0104_OUTPUTS_ENABLED 0
#endif

static void nts0104_preflight_before_bridge(void)
{
#if KLIMA_NTS0104_PREFLIGHT
    bridge_service_set_hardware_ready(false);
    nts0104_preflight_config_t config;
    nts0104_preflight_default_config(&config);
    /* OE is asserted low by init before any UART/ETM route is configured. */
    esp_err_t result = nts0104_preflight_init(&config);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "NTS0104 init failed; OE remains low: %s", esp_err_to_name(result));
    } else {
        ESP_LOGI(TAG, "NTS0104 preflight armed; OE held low while bridge starts");
    }
#endif
}

static void nts0104_preflight_after_bridge(void)
{
#if KLIMA_NTS0104_PREFLIGHT
#if !KLIMA_NTS0104_OUTPUTS_ENABLED
    /* SNIFFER/PASSIVE is intentionally a no-drive image. */
    (void)nts0104_preflight_disable();
    bridge_service_set_hardware_ready(false);
    ESP_LOGI(TAG, "NTS0104 remains disabled in RX-only profile");
    telemetry_publishf("NTS0104,event=disabled,reason=rx_only_profile");
    return;
#else
    esp_err_t result = nts0104_preflight_self_test();
    if (result == ESP_OK && nts0104_preflight_can_enable()) {
        result = nts0104_preflight_enable();
    } else if (result == ESP_OK) {
        result = ESP_ERR_INVALID_STATE;
    }
    if (result != ESP_OK) {
        (void)nts0104_preflight_disable();
        bridge_service_set_hardware_ready(false);
        nts0104_preflight_status_t status;
        nts0104_preflight_get_status(&status);
        ESP_LOGW(TAG, "NTS0104 remains disabled: reason=%s VDDA=%umV VDDB=%umV",
                 nts0104_preflight_reason_name(status.reason),
                 (unsigned)status.vdda_mv, (unsigned)status.vddb_mv);
        telemetry_publishf("NTS0104,event=disabled,reason=%s,vdda_mv=%u,vddb_mv=%u",
                           nts0104_preflight_reason_name(status.reason),
                           (unsigned)status.vdda_mv, (unsigned)status.vddb_mv);
    } else {
        bridge_service_set_hardware_ready(true);
        nts0104_preflight_status_t status;
        nts0104_preflight_get_status(&status);
        ESP_LOGI(TAG, "NTS0104 enabled: VDDA=%umV VDDB=%umV",
                 (unsigned)status.vdda_mv, (unsigned)status.vddb_mv);
        telemetry_publishf("NTS0104,event=enabled,vdda_mv=%u,vddb_mv=%u",
                           (unsigned)status.vdda_mv, (unsigned)status.vddb_mv);
    }
#endif
#endif
}

void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
    ESP_ERROR_CHECK(telemetry_init());
    ESP_ERROR_CHECK(capture_store_init());

    /* Bring up the recovery plane before touching the appliance link. A
     * bridge-driver fault must never make HTTP diagnostics and OTA
     * unreachable. esp_wifi_start() is asynchronous; the UART bridge still
     * starts immediately and does not wait for DHCP. */
#if KLIMA_HA_ZIGBEE_ACTIVE
    ESP_ERROR_CHECK(esp_coex_wifi_i154_enable());
#endif
    ESP_ERROR_CHECK(wifi_service_start());
    ESP_ERROR_CHECK(web_service_start());

#if KLIMA_NTS0104_PREFLIGHT
    nts0104_preflight_before_bridge();
#endif
    ESP_LOGI(TAG, "starting bridge profile: %s", bridge_service_profile_name());
    /* UART comes up before networking so a real MITM build cannot miss the
     * appliance's first exchange. The default build remains RX-only. */
    const esp_err_t bridge_result = bridge_service_start();
    if (bridge_result != ESP_OK) {
#if KLIMA_NTS0104_PREFLIGHT
        (void)nts0104_preflight_disable();
        bridge_service_set_hardware_ready(false);
#endif
        /* Keep diagnostics, OTA and MQTT reachable while every translator
         * channel remains fail-closed. A bridge initialization fault must
         * never turn into an opaque reboot loop. */
        ESP_LOGE(TAG, "bridge start failed; continuing diagnostics: %s",
                 esp_err_to_name(bridge_result));
        telemetry_publishf("BRIDGE,event=start_failed,error=%s",
                           esp_err_to_name(bridge_result));
    }
#if KLIMA_NTS0104_PREFLIGHT
    if (bridge_result == ESP_OK) {
        nts0104_preflight_after_bridge();
    }
#endif
#if KLIMA_HA_MQTT_ACTIVE
    ESP_ERROR_CHECK(mqtt_service_start());
#endif
#if KLIMA_HA_ZIGBEE_ACTIVE
    ESP_ERROR_CHECK(zigbee_service_start());
#endif

    const esp_err_t ota_validation = esp_ota_mark_app_valid_cancel_rollback();
    if (ota_validation != ESP_OK && ota_validation != ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
        ESP_ERROR_CHECK(ota_validation);
    }
}
