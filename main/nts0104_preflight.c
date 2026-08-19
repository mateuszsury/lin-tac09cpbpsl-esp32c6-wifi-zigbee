#include "nts0104_preflight.h"

#include "esp_check.h"

typedef struct {
    nts0104_preflight_config_t config;
    nts0104_preflight_status_t status;
} nts0104_context_t;

static nts0104_context_t s_ctx;

void nts0104_preflight_default_config(nts0104_preflight_config_t *config)
{
    if (config == NULL) {
        return;
    }
    *config = (nts0104_preflight_config_t){
        .oe_gpio = NTS0104_DEFAULT_OE_GPIO,
        .vdda_mv = NTS0104_REQUIRED_VDDA_MV,
        .vddb_mv = NTS0104_REQUIRED_VDDB_MV,
    };
}

static void set_reason(nts0104_preflight_reason_t reason)
{
    s_ctx.status.reason = reason;
}

static void close_oe(void)
{
    if (s_ctx.status.initialized) {
        (void)gpio_set_level(s_ctx.config.oe_gpio, 0);
        (void)gpio_set_direction(s_ctx.config.oe_gpio, GPIO_MODE_INPUT_OUTPUT);
    }
    s_ctx.status.oe_enabled = false;
}

static bool supply_contract_is_valid(const nts0104_preflight_config_t *config)
{
    return config->vdda_mv == NTS0104_REQUIRED_VDDA_MV &&
        config->vddb_mv == NTS0104_REQUIRED_VDDB_MV &&
        config->vdda_mv <= config->vddb_mv;
}

esp_err_t nts0104_preflight_init(const nts0104_preflight_config_t *config)
{
    nts0104_preflight_config_t defaults;
    nts0104_preflight_default_config(&defaults);
    s_ctx = (nts0104_context_t){
        .config = config != NULL ? *config : defaults,
    };

    if (s_ctx.config.oe_gpio < 0 || s_ctx.config.oe_gpio >= GPIO_NUM_MAX ||
        s_ctx.config.vdda_mv == 0U || s_ctx.config.vddb_mv == 0U) {
        set_reason(NTS0104_REASON_NOT_INITIALIZED);
        return ESP_ERR_INVALID_ARG;
    }

    const gpio_config_t oe_config = {
        .pin_bit_mask = 1ULL << s_ctx.config.oe_gpio,
        /* Keep the input path enabled so OE read-back is meaningful on
         * ESP32-C6; output-only mode makes gpio_get_level() read low. */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&oe_config), "nts0104", "configure OE");

    s_ctx.status.initialized = true;
    s_ctx.status.oe_gpio = s_ctx.config.oe_gpio;
    s_ctx.status.vdda_mv = s_ctx.config.vdda_mv;
    s_ctx.status.vddb_mv = s_ctx.config.vddb_mv;
    s_ctx.status.supply_contract_valid = supply_contract_is_valid(&s_ctx.config);
    close_oe();

    if (!s_ctx.status.supply_contract_valid) {
        set_reason(s_ctx.config.vdda_mv > s_ctx.config.vddb_mv
                       ? NTS0104_REASON_VDDA_GT_VDDB
                       : NTS0104_REASON_SUPPLY_CONTRACT_INVALID);
        return ESP_ERR_INVALID_STATE;
    }
    set_reason(NTS0104_REASON_OE_HELD_LOW);
    return ESP_OK;
}

esp_err_t nts0104_preflight_self_test(void)
{
    if (!s_ctx.status.initialized) {
        set_reason(NTS0104_REASON_NOT_INITIALIZED);
        return ESP_ERR_INVALID_STATE;
    }
    close_oe();
    if (!s_ctx.status.supply_contract_valid ||
        gpio_get_level(s_ctx.config.oe_gpio) != 0) {
        s_ctx.status.self_test_passed = false;
        set_reason(NTS0104_REASON_SELF_TEST_FAILED);
        return ESP_FAIL;
    }
    s_ctx.status.self_test_passed = true;
    set_reason(NTS0104_REASON_READY);
    return ESP_OK;
}

bool nts0104_preflight_can_enable(void)
{
    return s_ctx.status.initialized && s_ctx.status.supply_contract_valid &&
        s_ctx.status.self_test_passed && !s_ctx.status.oe_enabled;
}

esp_err_t nts0104_preflight_enable(void)
{
    if (!nts0104_preflight_can_enable()) {
        close_oe();
        if (!s_ctx.status.initialized) {
            set_reason(NTS0104_REASON_NOT_INITIALIZED);
        }
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = gpio_set_level(s_ctx.config.oe_gpio, 1);
    if (result != ESP_OK || gpio_get_level(s_ctx.config.oe_gpio) != 1) {
        close_oe();
        set_reason(NTS0104_REASON_SELF_TEST_FAILED);
        return result == ESP_OK ? ESP_FAIL : result;
    }
    s_ctx.status.oe_enabled = true;
    set_reason(NTS0104_REASON_ENABLED);
    return ESP_OK;
}

esp_err_t nts0104_preflight_disable(void)
{
    if (!s_ctx.status.initialized) {
        set_reason(NTS0104_REASON_NOT_INITIALIZED);
        return ESP_ERR_INVALID_STATE;
    }
    close_oe();
    set_reason(NTS0104_REASON_OE_HELD_LOW);
    return ESP_OK;
}

void nts0104_preflight_get_status(nts0104_preflight_status_t *status)
{
    if (status != NULL) {
        *status = s_ctx.status;
    }
}

const char *nts0104_preflight_reason_name(nts0104_preflight_reason_t reason)
{
    switch (reason) {
    case NTS0104_REASON_NOT_INITIALIZED: return "not_initialized";
    case NTS0104_REASON_OE_HELD_LOW: return "oe_held_low";
    case NTS0104_REASON_SUPPLY_CONTRACT_INVALID: return "supply_contract_invalid";
    case NTS0104_REASON_VDDA_GT_VDDB: return "vdda_gt_vddb";
    case NTS0104_REASON_SELF_TEST_FAILED: return "self_test_failed";
    case NTS0104_REASON_READY: return "ready";
    case NTS0104_REASON_ENABLED: return "enabled";
    case NTS0104_REASON_NONE:
    default: return "none";
    }
}
