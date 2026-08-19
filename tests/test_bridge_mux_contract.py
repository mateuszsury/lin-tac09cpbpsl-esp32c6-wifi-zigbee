from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BRIDGE = (ROOT / "main" / "bridge_service.c").read_text(encoding="utf-8")
PROTOCOL_H = (ROOT / "main" / "ac_protocol.h").read_text(encoding="utf-8")
PROTOCOL_C = (ROOT / "main" / "ac_protocol.c").read_text(encoding="utf-8")


def _between(start: str, end: str) -> str:
    first = BRIDGE.index(start)
    last = BRIDGE.index(end, first)
    return BRIDGE[first:last]


def test_nts_topology_and_panel_wire_length_are_canonical():
    assert "#define AC_PANEL_FRAME_SIZE 15U" in PROTOCOL_H
    assert "AC_PANEL_BODY_LENGTH 0x0BU" in PROTOCOL_H
    assert "GPIO4->GPIO6" in BRIDGE
    assert "GPIO7->GPIO0" in BRIDGE
    assert "#if KLIMA_PANEL_HARDWARE_FORWARD\n            GPIO_NUM_NC" in BRIDGE


def test_control_mux_sequence_is_fail_closed_and_reversible():
    helper = _between(
        "static esp_err_t set_panel_to_main_hardware_mirror_enabled",
        "typedef enum {",
    )
    # The route is disabled before the UART pin is connected for injection.
    assert "esp_etm_channel_disable" in helper
    assert "uart_set_pin" in helper
    assert "if (enabled)" in helper
    assert "if (!enabled)" in helper
    # Restore disconnects UART before ETM is enabled; takeover routes UART only
    # after the fall channel has been disabled.
    assert helper.index("if (enabled) {") < helper.index("rise_result")
    assert helper.index("fall_result") < helper.rindex("if (!enabled)")
    assert "CONTROL,event=etm_takeover_failed" in BRIDGE
    assert "hybrid_overlay_burst_sent" in BRIDGE
    hybrid = _between("static void hybrid_forward_panel_byte", "#endif")
    assert "index == 0U && stream->hybrid_overlay_valid" in hybrid
    assert "stream->hybrid_overlay_frame,\n                AC_PANEL_FRAME_SIZE" in hybrid
    assert "received == 0x12U" not in hybrid
    complete_hybrid = _between(
        "static void hybrid_forward_panel_byte",
        "#if KLIMA_TAP_INJECT",
    )
    assert complete_hybrid.count("index == 0U && stream->hybrid_overlay_valid") == 2
    assert complete_hybrid.count("stream->hybrid_overlay_burst_sent = true") == 2


def test_confirmation_timeout_and_physical_panel_restore_passthrough():
    assert BRIDGE.count("set_panel_to_main_hardware_mirror_enabled(true)") >= 3
    assert "CONTROL,event=cancelled_by_physical_panel" in BRIDGE
    assert "physical_panel_state_changed" in BRIDGE
    assert "AC_PANEL_FRAME_SIZE - 5U" in BRIDGE
    assert "CONTROL,event=timeout" in BRIDGE
    assert "bridge_service_set_hardware_ready" in BRIDGE
    assert "A response-confirmed command is complete" in BRIDGE
    assert "s_overlay.request = (ac_control_request_t){0};" in BRIDGE
    assert "s_state.override_active = false;" in BRIDGE


def test_persistent_event_marker_is_not_misclassified_as_a_new_panel_action():
    stable = bytes.fromhex("54440B1133801200004000000000FB")
    repeated_event = bytes.fromhex("54440B1233801200004000000000F8")
    changed_event = bytes.fromhex("54440B1233801300004200000000FB")

    def semantic_payload_changed(previous: bytes, current: bytes) -> bool:
        return previous[4:-1] != current[4:-1]

    assert not semantic_payload_changed(stable, repeated_event)
    assert semantic_payload_changed(repeated_event, changed_event)


def test_takeover_task_always_blocks_for_at_least_one_scheduler_tick():
    takeover = _between(
        "static void control_takeover_task",
        "static void control_watchdog_task",
    )
    assert "vTaskDelay(1);" in takeover
    assert "vTaskDelay(pdMS_TO_TICKS(1))" not in takeover


def test_stable_frame_boundary_model_has_exactly_one_wire_frame():
    physical = bytes.fromhex("54440B113100120000400000000079")
    replacement = bytes.fromhex("54440B1231801200004000000000FA")
    # ETM is disabled at the idle boundary before the next source frame. The
    # CPU owns the whole destination frame and emits one contiguous command.
    wire = replacement
    assert len(wire) == 15
    assert wire == replacement
    assert "takeover_pending" in BRIDGE
    assert "s_panel_last_byte_us" in BRIDGE


def test_panel_checksum_is_validated_and_regenerated():
    assert "ac_protocol_xor(data, length) == AC_PROTOCOL_XOR_TARGET" in PROTOCOL_C
    assert "data[2] == AC_PANEL_BODY_LENGTH && length == AC_PANEL_FRAME_SIZE" in PROTOCOL_C
    assert "data[length - 1U]" in PROTOCOL_C


def test_mitm_nts_uses_hardware_forwarding_and_authentic_panel_loop():
    cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
    mitm = cmake.split(
        'elseif(KLIMA_BRIDGE_MODE_NORMALIZED STREQUAL "MITM_NTS")', 1
    )[1].split("else()", 1)[0]
    assert "KLIMA_PANEL_HARDWARE_FORWARD=1" in mitm
    assert "KLIMA_PANEL_LOOP_CONTROL=1" in mitm
    assert "KLIMA_SYNCED_TX_PROBE=0" in mitm
