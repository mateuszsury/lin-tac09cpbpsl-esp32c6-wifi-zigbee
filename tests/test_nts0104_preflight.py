from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "main" / "nts0104_preflight.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "main" / "nts0104_preflight.c").read_text(encoding="utf-8")
HARDWARE = (ROOT / "docs" / "HARDWARE.md").read_text(encoding="utf-8")


def test_pin_contract_and_oe_interlock_are_documented():
    for token in ("GPIO4", "GPIO6", "GPIO7", "GPIO0", "GPIO3", "10 kOhm"):
        assert token in HARDWARE
    assert "NTS0104_DEFAULT_OE_GPIO GPIO_NUM_3" in HEADER
    assert "gpio_set_level(s_ctx.config.oe_gpio, 0)" in SOURCE


def test_supply_contract_is_fixed_to_installed_hardware():
    assert "NTS0104_REQUIRED_VDDA_MV 3300U" in HEADER
    assert "NTS0104_REQUIRED_VDDB_MV 5000U" in HEADER
    assert "supply_contract_is_valid" in SOURCE
    assert "config->vdda_mv <= config->vddb_mv" in SOURCE
    assert "NTS0104_REASON_SUPPLY_CONTRACT_INVALID" in HEADER
    assert "NTS0104_REASON_VDDA_GT_VDDB" in HEADER


def test_fail_closed_api_and_enable_gate_exist():
    for symbol in (
        "nts0104_preflight_init",
        "nts0104_preflight_self_test",
        "nts0104_preflight_enable",
        "nts0104_preflight_disable",
        "nts0104_preflight_can_enable",
        "nts0104_preflight_get_status",
    ):
        assert symbol in HEADER
        assert symbol in SOURCE
    assert "s_ctx.status.supply_contract_valid" in SOURCE
    assert "s_ctx.status.self_test_passed" in SOURCE
    assert "close_oe();" in SOURCE
    assert "GPIO_MODE_INPUT_OUTPUT" in SOURCE
    assert "GPIO_MODE_OUTPUT);" not in SOURCE


def test_final_topology_has_no_bypass_jumpers():
    assert "no bypass jumpers" in HARDWARE.lower()
    assert "GPIO4-GPIO6" in HARDWARE
    assert "GPIO0-GPIO7" in HARDWARE
