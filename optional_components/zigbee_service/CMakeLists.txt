idf_component_register(
    SRCS "zigbee_service.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "../../main"
    REQUIRES esp_app_format esp_coex espressif__esp-zigbee-lib nvs_flash
)
