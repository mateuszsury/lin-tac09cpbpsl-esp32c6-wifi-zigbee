#include "wifi_service.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_check.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "klima_secrets.h"
#include "mdns.h"
#include "telemetry.h"

#ifndef KLIMA_WIFI_FALLBACK_AP_ENABLED
#define KLIMA_WIFI_FALLBACK_AP_ENABLED 1
#endif

static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_service_status_t s_status;

static void network_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        portENTER_CRITICAL(&s_status_lock);
        s_status.sta_connected = false;
        s_status.sta_ip[0] = '\0';
        portEXIT_CRITICAL(&s_status_lock);
        telemetry_publishf("WIFI,event=disconnected,reconnect=1");
        esp_wifi_connect();
        return;
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        portENTER_CRITICAL(&s_status_lock);
        s_status.sta_connected = true;
        strlcpy(s_status.sta_ip, ip, sizeof(s_status.sta_ip));
        portEXIT_CRITICAL(&s_status_lock);
        telemetry_publishf("WIFI,event=connected,ip=%s,hostname=klima-wifi.local", ip);
    }
}

esp_err_t wifi_service_start(void)
{
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), "wifi", "read MAC");

    const size_t ap_password_length = strlen(KLIMA_AP_PASSWORD);
    const bool ap_enabled = KLIMA_WIFI_FALLBACK_AP_ENABLED &&
        ap_password_length >= 12U && ap_password_length <= 63U;

    portENTER_CRITICAL(&s_status_lock);
    s_status.ap_enabled = ap_enabled;
    if (ap_enabled) {
        snprintf(
            s_status.ap_ssid,
            sizeof(s_status.ap_ssid),
            "KlimaWiFi-%02X%02X",
            mac[4],
            mac[5]);
        strlcpy(
            s_status.ap_password,
            KLIMA_AP_PASSWORD,
            sizeof(s_status.ap_password));
    } else {
        s_status.ap_ssid[0] = '\0';
        s_status.ap_password[0] = '\0';
    }
    portEXIT_CRITICAL(&s_status_lock);

    ESP_RETURN_ON_ERROR(esp_netif_init(), "wifi", "netif init");
    esp_err_t event_loop_result = esp_event_loop_create_default();
    if (event_loop_result != ESP_OK && event_loop_result != ESP_ERR_INVALID_STATE) {
        return event_loop_result;
    }

    esp_netif_t *ap_netif = ap_enabled
        ? esp_netif_create_default_wifi_ap()
        : NULL;
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif || (ap_enabled && !ap_netif)) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(sta_netif, "klima-wifi"), "wifi", "hostname");

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), "wifi", "wifi init");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event_handler, NULL),
        "wifi",
        "wifi event handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event_handler, NULL),
        "wifi",
        "ip event handler");

    wifi_service_status_t status;
    wifi_service_get_status(&status);
    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, status.ap_ssid, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, status.ap_password, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(status.ap_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.pmf_cfg.required = false;

    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, KLIMA_WIFI_SSID, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, KLIMA_WIFI_PASSWORD, sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(
        esp_wifi_set_mode(ap_enabled ? WIFI_MODE_APSTA : WIFI_MODE_STA),
        "wifi",
        "wifi mode");
    if (ap_enabled) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), "wifi", "ap config");
    }
    if (strlen(KLIMA_WIFI_SSID) > 0U) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config), "wifi", "sta config");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), "wifi", "wifi start");
    ESP_RETURN_ON_ERROR(
        esp_wifi_set_ps(
            ap_enabled ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM),
        "wifi",
        "power save");

    esp_err_t mdns_result = mdns_init();
    if (mdns_result == ESP_OK) {
        mdns_hostname_set("klima-wifi");
        mdns_instance_name_set("Klima WiFi protocol probe");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    } else {
        telemetry_publishf("WIFI,event=mdns_error,code=%s", esp_err_to_name(mdns_result));
    }

    telemetry_publishf(
        "WIFI,event=started,ap_enabled=%d,ap_ssid=%s,ap_ip=%s,sta_ssid=%s",
        ap_enabled,
        ap_enabled ? status.ap_ssid : "disabled",
        ap_enabled ? "192.168.4.1" : "disabled",
        strlen(KLIMA_WIFI_SSID) ? KLIMA_WIFI_SSID : "disabled");
    return ESP_OK;
}

void wifi_service_get_status(wifi_service_status_t *status)
{
    if (!status) {
        return;
    }
    portENTER_CRITICAL(&s_status_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
}
