from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def profile_block(name: str) -> str:
    source = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
    marker = f'KLIMA_BRIDGE_MODE_NORMALIZED STREQUAL "{name}"'
    start = source.index(marker)
    end_candidates = [
        position for token in ("elseif(", "else()")
        if (position := source.find(token, start + len(marker))) >= 0
    ]
    return source[start:min(end_candidates)]


def test_zigbee_is_an_optional_transport_component():
    root_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    main_cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "optional_components/zigbee_service" in root_cmake
    assert "list(APPEND COMPONENTS zigbee_service)" in root_cmake
    assert "zigbee_service" not in main_cmake


def test_profiles_have_separate_sdkconfig_inputs():
    root_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    base = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
    zigbee = (ROOT / "sdkconfig.zigbee.defaults").read_text(encoding="utf-8")
    assert 'set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")' in root_cmake
    assert "sdkconfig.zigbee.defaults" in root_cmake
    assert "CONFIG_ZB_ENABLED" not in base
    assert "CONFIG_ZB_ENABLED=y" in zigbee
    assert "CONFIG_IEEE802154_ENABLED=y" in zigbee


def test_managed_dependencies_are_exactly_pinned():
    mqtt = (ROOT / "main" / "idf_component.yml").read_text(encoding="utf-8")
    zigbee = (
        ROOT / "optional_components" / "zigbee_service" / "idf_component.yml"
    ).read_text(encoding="utf-8")
    assert 'espressif/mdns: "1.11.3"' in mqtt
    assert 'espressif/esp-zigbee-lib: "2.0.3"' in zigbee
    assert 'version: "5.3.2"' in zigbee


def test_public_profile_surface_is_deliberately_small():
    source = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
    for name in ("SNIFFER", "PASSIVE", "PANEL_DIAG", "PANEL_BENCH", "BRIDGE", "MITM_NTS"):
        assert name in source
    for removed in ("TAP", "HEADLESS", "HYBRID_HIL", "INPUTS_ALT7", "GPIO0_UART_DIAG"):
        assert f'STREQUAL "{removed}"' not in source


def test_safe_default_is_rx_only_and_oe_low():
    root = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    profile = profile_block("SNIFFER")
    assert 'set(KLIMA_BRIDGE_MODE "SNIFFER")' in root
    assert "KLIMA_BRIDGE_ACTIVE=0" in profile
    assert "KLIMA_NTS0104_OUTPUTS_ENABLED=0" in profile
    assert "KLIMA_CANONICAL_SNIFFER=1" in profile


def test_panel_profiles_are_explicit_and_detached_only():
    diag = profile_block("PANEL_DIAG")
    bench = profile_block("PANEL_BENCH")
    assert "KLIMA_BRIDGE_ACTIVE=0" in diag
    assert "KLIMA_CANONICAL_PANEL_DIAG=1" in diag
    assert "KLIMA_PANEL_BENCH_EMULATOR=1" in bench
    assert "Never use while the real main PCB is attached" in bench


def test_bridge_and_mitm_use_final_gpio_map():
    cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
    for definition in (
        "KLIMA_MAIN_RX_GPIO_NUM=4", "KLIMA_PANEL_TX_GPIO_NUM=6",
        "KLIMA_PANEL_RX_GPIO_NUM=7", "KLIMA_MAIN_TX_GPIO_NUM=0",
    ):
        assert definition in cmake
    bridge = profile_block("BRIDGE")
    mitm = profile_block("MITM_NTS")
    assert "KLIMA_CONTROL_VERIFIED=0" in bridge
    assert "KLIMA_CONTROL_VERIFIED=1" in mitm
    assert "KLIMA_PANEL_HARDWARE_FORWARD=1" in mitm
    assert "KLIMA_PANEL_LOOP_CONTROL=1" in mitm
    assert "KLIMA_A4_B4_DIAGNOSTIC=0" in mitm


def test_bridge_start_failure_keeps_diagnostics_reachable_fail_closed():
    source = (ROOT / "main" / "main.c").read_text(encoding="utf-8")
    assert "const esp_err_t bridge_result = bridge_service_start();" in source
    assert "BRIDGE,event=start_failed,error=%s" in source
    assert "bridge_service_set_hardware_ready(false);" in source
    assert source.index("wifi_service_start()") < source.index("bridge_service_start()")
    assert source.index("web_service_start()") < source.index("bridge_service_start()")
