#pragma once

/*
 * OE interlock for the KAmodNTS0104PW level translator.
 *
 * The installed hardware contract is fixed: VDD(A)=3.3 V, VDD(B)=5 V and
 * OE=GPIO3 with an external 10 kOhm pull-down. There is intentionally no ADC
 * or rail-sensing input in this module.
 */

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NTS0104_DEFAULT_OE_GPIO GPIO_NUM_3
#define NTS0104_REQUIRED_VDDA_MV 3300U
#define NTS0104_REQUIRED_VDDB_MV 5000U

typedef enum {
    NTS0104_REASON_NONE = 0,
    NTS0104_REASON_NOT_INITIALIZED,
    NTS0104_REASON_OE_HELD_LOW,
    NTS0104_REASON_SUPPLY_CONTRACT_INVALID,
    NTS0104_REASON_VDDA_GT_VDDB,
    NTS0104_REASON_SELF_TEST_FAILED,
    NTS0104_REASON_READY,
    NTS0104_REASON_ENABLED,
} nts0104_preflight_reason_t;

typedef struct {
    gpio_num_t oe_gpio;
    uint32_t vdda_mv;
    uint32_t vddb_mv;
} nts0104_preflight_config_t;

typedef struct {
    bool initialized;
    bool oe_enabled;
    bool supply_contract_valid;
    bool self_test_passed;
    nts0104_preflight_reason_t reason;
    gpio_num_t oe_gpio;
    uint32_t vdda_mv;
    uint32_t vddb_mv;
} nts0104_preflight_status_t;

void nts0104_preflight_default_config(nts0104_preflight_config_t *config);

/* Configure OE as an output and drive it LOW before bridge pins are routed. */
esp_err_t nts0104_preflight_init(const nts0104_preflight_config_t *config);

/* Verify the declared 3.3 V / 5 V contract and that OE is physically LOW. */
esp_err_t nts0104_preflight_self_test(void);

/* Raise OE only after init and self-test. */
esp_err_t nts0104_preflight_enable(void);

/* Immediately place all translator channels in high impedance (OE LOW). */
esp_err_t nts0104_preflight_disable(void);

bool nts0104_preflight_can_enable(void);
void nts0104_preflight_get_status(nts0104_preflight_status_t *status);
const char *nts0104_preflight_reason_name(nts0104_preflight_reason_t reason);

#ifdef __cplusplus
}
#endif
