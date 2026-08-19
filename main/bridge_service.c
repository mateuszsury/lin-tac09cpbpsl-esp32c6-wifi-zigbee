#include "bridge_service.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/gpio_etm.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "driver/uart.h"
#include "capture_store.h"
#include "esp_check.h"
#include "esp_etm.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "hal/gpio_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "telemetry.h"

#ifndef KLIMA_BRIDGE_ACTIVE
#define KLIMA_BRIDGE_ACTIVE 0
#endif
#ifndef KLIMA_BRIDGE_MIRROR_PROBE
#define KLIMA_BRIDGE_MIRROR_PROBE 0
#endif
#ifndef KLIMA_HEADLESS_PROBE
#define KLIMA_HEADLESS_PROBE 0
#endif
#ifndef KLIMA_MAIN_EDGE_FORWARD
#define KLIMA_MAIN_EDGE_FORWARD 0
#endif
#ifndef KLIMA_MAIN_HARDWARE_FORWARD
#define KLIMA_MAIN_HARDWARE_FORWARD 0
#endif
#ifndef KLIMA_TX_OPEN_DRAIN
#define KLIMA_TX_OPEN_DRAIN 0
#endif
#ifndef KLIMA_MAIN_TX_OPEN_DRAIN
#define KLIMA_MAIN_TX_OPEN_DRAIN 0
#endif
#ifndef KLIMA_TAP_INJECT
#define KLIMA_TAP_INJECT 0
#endif
#ifndef KLIMA_HYBRID_BRIDGE
#define KLIMA_HYBRID_BRIDGE 0
#endif
#ifndef KLIMA_CONTROL_VERIFIED
#define KLIMA_CONTROL_VERIFIED KLIMA_BRIDGE_ACTIVE
#endif
#ifndef KLIMA_SYNCED_TX_PROBE
#define KLIMA_SYNCED_TX_PROBE 0
#endif
#ifndef KLIMA_PANEL_HARDWARE_FORWARD
#define KLIMA_PANEL_HARDWARE_FORWARD 0
#endif
#ifndef KLIMA_PANEL_LOOP_CONTROL
#define KLIMA_PANEL_LOOP_CONTROL 0
#endif
#ifndef KLIMA_BOOT_FORCE_OFF_DIAG
#define KLIMA_BOOT_FORCE_OFF_DIAG 0
#endif
#ifndef KLIMA_A4_B4_DIAGNOSTIC
#define KLIMA_A4_B4_DIAGNOSTIC 0
#endif
#define A4_MONITOR_GPIO GPIO_NUM_5
#define B4_MONITOR_GPIO GPIO_NUM_1
#ifndef KLIMA_CONVERTER_LOOPBACK_PROBE
#define KLIMA_CONVERTER_LOOPBACK_PROBE 0
#endif
#define CONVERTER_LOOPBACK_GPIO GPIO_NUM_3
#ifndef KLIMA_HEADLESS_MAIN22_PROBE
#define KLIMA_HEADLESS_MAIN22_PROBE 0
#endif
#ifndef KLIMA_HARDWARE_BYPASS_PROBE
#define KLIMA_HARDWARE_BYPASS_PROBE 0
#endif
#ifndef KLIMA_ALL_INPUTS_PROBE
#define KLIMA_ALL_INPUTS_PROBE 0
#endif
#ifndef KLIMA_WAKE7_PROBE
#define KLIMA_WAKE7_PROBE 0
#endif
#ifndef KLIMA_WAKE_DEST_GPIO_NUM
#define KLIMA_WAKE_DEST_GPIO_NUM 7
#endif
#ifndef KLIMA_WAKE_ALT_GPIO_NUM
#define KLIMA_WAKE_ALT_GPIO_NUM 6
#endif
#define WAKE_DEST_GPIO ((gpio_num_t)KLIMA_WAKE_DEST_GPIO_NUM)
#define WAKE_ALT_GPIO ((gpio_num_t)KLIMA_WAKE_ALT_GPIO_NUM)
#ifndef KLIMA_VIRTUAL_PANEL_WIRE_SIZE
#define KLIMA_VIRTUAL_PANEL_WIRE_SIZE AC_PANEL_FRAME_SIZE
#endif
#ifndef KLIMA_VIRTUAL_PANEL_EARLY_BOOT
#define KLIMA_VIRTUAL_PANEL_EARLY_BOOT 0
#endif
#ifndef KLIMA_NTS0104_PREFLIGHT
#define KLIMA_NTS0104_PREFLIGHT 0
#endif
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
#ifndef KLIMA_PANEL_BENCH_EMULATOR
#define KLIMA_PANEL_BENCH_EMULATOR 0
#endif
#ifndef KLIMA_GPIO0_UART_DIAG
#define KLIMA_GPIO0_UART_DIAG 0
#endif

#define BRIDGE_BAUD_RATE 9600
#define BRIDGE_RX_BUFFER_SIZE 2048
#define BRIDGE_TX_BUFFER_SIZE 1024
#define BRIDGE_READ_CHUNK_SIZE 128
#define BRIDGE_STREAM_TIMEOUT_US 30000LL
#define BRIDGE_LINK_FRESH_MS 1000LL
#define BRIDGE_COMMAND_CONFIRM_TIMEOUT_MS 5000LL
#define BRIDGE_VIRTUAL_PANEL_START_MS 1500LL
#define BRIDGE_VIRTUAL_PANEL_PERIOD_MS 100U
#define SYNC_TX_RESOLUTION_HZ 10000000U
#define SYNC_TX_AFTER_MAIN_US 30500ULL
#define SYNC_TX_SCAN_START_US 5000ULL
#define SYNC_TX_SCAN_STEP_US 1000ULL
#define SYNC_TX_SCAN_SLOTS 50U
#define SYNC_TX_UART_BITS (AC_PANEL_FRAME_SIZE * 10U)
#define SYNC_TX_SYMBOLS (SYNC_TX_UART_BITS / 2U)

/* Final KAmodNTS0104PW topology (no jumpers):
 *
 *   main TX  -> GPIO4 (ESP RX) -> GPIO6 (ESP TX) -> panel RX
 *   panel TX -> GPIO7 (ESP RX) -> GPIO0 (ESP TX) -> main RX
 *
 * GPIO4->GPIO6 and GPIO7->GPIO0 are the transparent ETM paths.  UARTs only
 * tap the source endpoints for framing/state; the CPU takes ownership of a
 * destination for one complete panel frame only while a queued command is
 * being injected and then returns it to ETM/pass-through operation.
 */
#define PANEL_UART UART_NUM_0
#ifndef KLIMA_PANEL_RX_GPIO_NUM
#define KLIMA_PANEL_RX_GPIO_NUM 7
#endif

#ifndef KLIMA_MAIN_RX_GPIO_NUM
#define KLIMA_MAIN_RX_GPIO_NUM 4
#endif
#ifndef KLIMA_MAIN_TX_GPIO_NUM
#define KLIMA_MAIN_TX_GPIO_NUM 0
#endif
#ifndef KLIMA_PANEL_TX_GPIO_NUM
#define KLIMA_PANEL_TX_GPIO_NUM 6
#endif
#define MAIN_TX_GPIO ((gpio_num_t)KLIMA_MAIN_TX_GPIO_NUM)
#define PANEL_TX_GPIO ((gpio_num_t)KLIMA_PANEL_TX_GPIO_NUM)
#define PANEL_RX_GPIO ((gpio_num_t)KLIMA_PANEL_RX_GPIO_NUM)
#define MAIN_UART UART_NUM_1
#define MAIN_RX_GPIO ((gpio_num_t)KLIMA_MAIN_RX_GPIO_NUM)

#if KLIMA_GPIO0_UART_DIAG
static volatile uint32_t s_gpio0_diag_tx_edges;
static volatile uint32_t s_gpio0_diag_a4_edges;

static void IRAM_ATTR gpio0_diag_edge_isr(void *arg)
{
    volatile uint32_t *counter = (volatile uint32_t *)arg;
    ++(*counter);
}

static void gpio0_uart_diag_task(void *arg)
{
    (void)arg;
    uint8_t pattern[64];
    memset(pattern, 0x55, sizeof(pattern));
    for (;;) {
        const uint32_t tx_before =
            __atomic_load_n(&s_gpio0_diag_tx_edges, __ATOMIC_RELAXED);
        const uint32_t a4_before =
            __atomic_load_n(&s_gpio0_diag_a4_edges, __ATOMIC_RELAXED);
        const int written = uart_write_bytes(MAIN_UART, pattern, sizeof(pattern));
        const esp_err_t drained = uart_wait_tx_done(MAIN_UART, pdMS_TO_TICKS(250));
        vTaskDelay(pdMS_TO_TICKS(20));
        const uint32_t tx_after =
            __atomic_load_n(&s_gpio0_diag_tx_edges, __ATOMIC_RELAXED);
        const uint32_t a4_after =
            __atomic_load_n(&s_gpio0_diag_a4_edges, __ATOMIC_RELAXED);
        telemetry_publishf(
            "GPIO0_UART_DIAG,written=%d,drained=%s,gpio0_edges=%" PRIu32
            ",a4_gpio5_edges=%" PRIu32 ",gpio0_level=%d,a4_level=%d",
            written,
            esp_err_to_name(drained),
            tx_after - tx_before,
            a4_after - a4_before,
            gpio_get_level(MAIN_TX_GPIO),
            gpio_get_level(A4_MONITOR_GPIO));
        vTaskDelay(pdMS_TO_TICKS(1980));
    }
}
#endif

/* The passive build listens directly on the same two factory transmitter
 * wires and keeps every GPIO4..GPIO7 pin high-impedance. */
#define PASSIVE_MAIN_RX_GPIO GPIO_NUM_4
#define PASSIVE_PANEL_RX_GPIO PANEL_RX_GPIO
#define PROBE_MAIN_RX_GPIO GPIO_NUM_4
#define PROBE_PANEL_RX_GPIO GPIO_NUM_6
#define PROBE_MAIN_TO_PANEL_GPIO GPIO_NUM_6
#define PROBE_PANEL_CANDIDATE_GPIO5 GPIO_NUM_6
#define PROBE_ALTERNATE_PANEL_RX_GPIO GPIO_NUM_7
#ifndef KLIMA_ALL_INPUTS_PANEL_RX_GPIO_NUM
#define KLIMA_ALL_INPUTS_PANEL_RX_GPIO_NUM 5
#endif
#ifndef KLIMA_ALL_INPUTS_MAIN_RX_GPIO_NUM
#define KLIMA_ALL_INPUTS_MAIN_RX_GPIO_NUM 6
#endif
#define ALL_INPUTS_PANEL_RX_GPIO ((gpio_num_t)KLIMA_ALL_INPUTS_PANEL_RX_GPIO_NUM)
#define ALL_INPUTS_MAIN_RX_GPIO ((gpio_num_t)KLIMA_ALL_INPUTS_MAIN_RX_GPIO_NUM)

#if KLIMA_HARDWARE_BYPASS_PROBE || KLIMA_MAIN_HARDWARE_FORWARD || \
    KLIMA_PANEL_HARDWARE_FORWARD
typedef struct {
    esp_etm_event_handle_t rise_event;
    esp_etm_event_handle_t fall_event;
    esp_etm_task_handle_t set_task;
    esp_etm_task_handle_t clear_task;
    esp_etm_channel_handle_t rise_channel;
    esp_etm_channel_handle_t fall_channel;
} hardware_mirror_t;

static hardware_mirror_t s_hardware_mirrors[2];
/* The panel->main ETM route is normally enabled.  A Wi-Fi command takes
 * ownership of the main-side output for one complete panel frame; this flag
 * is intentionally separate from the command state so the physical panel can
 * win and immediately restore the transparent route. */
static bool s_panel_to_main_hardware_mirror_enabled;

static esp_err_t start_hardware_mirror(
    gpio_num_t source,
    gpio_num_t destination,
    hardware_mirror_t *mirror)
{
    ESP_RETURN_ON_ERROR(
        gpio_reset_pin(destination),
        "bridge",
        "reset ETM destination pad");
    gpio_set_level(destination, gpio_get_level(source));
    ESP_RETURN_ON_ERROR(
        gpio_set_direction(destination, GPIO_MODE_OUTPUT),
        "bridge",
        "ETM mirror output");

    gpio_etm_event_config_t event_config = {0};
    event_config.edges[0] = GPIO_ETM_EVENT_EDGE_POS;
    event_config.edges[1] = GPIO_ETM_EVENT_EDGE_NEG;
    ESP_RETURN_ON_ERROR(
        gpio_new_etm_event(
            &event_config, &mirror->rise_event, &mirror->fall_event),
        "bridge",
        "create GPIO ETM events");
    ESP_RETURN_ON_ERROR(
        gpio_etm_event_bind_gpio(mirror->rise_event, source),
        "bridge",
        "bind rising GPIO ETM event");
    ESP_RETURN_ON_ERROR(
        gpio_etm_event_bind_gpio(mirror->fall_event, source),
        "bridge",
        "bind falling GPIO ETM event");

    gpio_etm_task_config_t task_config = {0};
    task_config.actions[0] = GPIO_ETM_TASK_ACTION_SET;
    task_config.actions[1] = GPIO_ETM_TASK_ACTION_CLR;
    ESP_RETURN_ON_ERROR(
        gpio_new_etm_task(
            &task_config, &mirror->set_task, &mirror->clear_task),
        "bridge",
        "create GPIO ETM tasks");
    ESP_RETURN_ON_ERROR(
        gpio_etm_task_add_gpio(mirror->set_task, destination),
        "bridge",
        "bind GPIO ETM set task");
    ESP_RETURN_ON_ERROR(
        gpio_etm_task_add_gpio(mirror->clear_task, destination),
        "bridge",
        "bind GPIO ETM clear task");

    const esp_etm_channel_config_t channel_config = {};
    ESP_RETURN_ON_ERROR(
        esp_etm_new_channel(&channel_config, &mirror->rise_channel),
        "bridge",
        "create rising ETM channel");
    ESP_RETURN_ON_ERROR(
        esp_etm_new_channel(&channel_config, &mirror->fall_channel),
        "bridge",
        "create falling ETM channel");
    ESP_RETURN_ON_ERROR(
        esp_etm_channel_connect(
            mirror->rise_channel, mirror->rise_event, mirror->set_task),
        "bridge",
        "connect rising ETM channel");
    ESP_RETURN_ON_ERROR(
        esp_etm_channel_connect(
            mirror->fall_channel, mirror->fall_event, mirror->clear_task),
        "bridge",
        "connect falling ETM channel");
    ESP_RETURN_ON_ERROR(
        esp_etm_channel_enable(mirror->rise_channel),
        "bridge",
        "enable rising ETM channel");
    ESP_RETURN_ON_ERROR(
        esp_etm_channel_enable(mirror->fall_channel),
        "bridge",
        "enable falling ETM channel");

    telemetry_publishf(
        "BYPASS,event=hardware_etm_started,source_gpio=%d,destination_gpio=%d",
        source,
        destination);
    return ESP_OK;
}

static esp_err_t set_panel_to_main_hardware_mirror_enabled(bool enabled)
{
    if (s_panel_to_main_hardware_mirror_enabled == enabled) {
        return ESP_OK;
    }
#if !KLIMA_SYNCED_TX_PROBE
    /* Main UART keeps a TX buffer installed but its pin is disconnected while
     * ETM owns MAIN_TX_GPIO. Disconnect it before enabling ETM; connect it only
     * after both ETM edge channels are disabled. */
    if (enabled) {
        const esp_err_t route_result = uart_set_pin(
            MAIN_UART,
            (gpio_num_t)UART_PIN_NO_CHANGE,
            MAIN_RX_GPIO,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE);
        if (route_result != ESP_OK) {
            return route_result;
        }
    }
#endif

    const esp_err_t rise_result = enabled
        ? esp_etm_channel_enable(s_hardware_mirrors[0].rise_channel)
        : esp_etm_channel_disable(s_hardware_mirrors[0].rise_channel);
    if (rise_result != ESP_OK) {
#if !KLIMA_SYNCED_TX_PROBE
        if (enabled) {
            (void)uart_set_pin(
                MAIN_UART, MAIN_TX_GPIO, MAIN_RX_GPIO,
                UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        }
#endif
        return rise_result;
    }
    const esp_err_t fall_result = enabled
        ? esp_etm_channel_enable(s_hardware_mirrors[0].fall_channel)
        : esp_etm_channel_disable(s_hardware_mirrors[0].fall_channel);
    if (fall_result != ESP_OK) {
        /* Best-effort rollback keeps both edge channels in the same state. */
        if (enabled) {
            (void)esp_etm_channel_disable(s_hardware_mirrors[0].rise_channel);
        } else {
            (void)esp_etm_channel_enable(s_hardware_mirrors[0].rise_channel);
        }
#if !KLIMA_SYNCED_TX_PROBE
        if (enabled) {
            (void)uart_set_pin(
                MAIN_UART, MAIN_TX_GPIO, MAIN_RX_GPIO,
                UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        }
#endif
        return fall_result;
    }
#if !KLIMA_SYNCED_TX_PROBE
    if (!enabled) {
        const esp_err_t route_result = uart_set_pin(
            MAIN_UART,
            MAIN_TX_GPIO,
            MAIN_RX_GPIO,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE);
        if (route_result != ESP_OK) {
            (void)esp_etm_channel_enable(s_hardware_mirrors[0].rise_channel);
            (void)esp_etm_channel_enable(s_hardware_mirrors[0].fall_channel);
            return route_result;
        }
    }
#endif
    s_panel_to_main_hardware_mirror_enabled = enabled;
    telemetry_publishf(
        "BYPASS,event=panel_to_main_etm_%s",
        enabled ? "enabled" : "disabled");
    return ESP_OK;
}
#endif

#if !KLIMA_PANEL_HARDWARE_FORWARD
static esp_err_t set_panel_to_main_hardware_mirror_enabled(bool enabled)
{
    (void)enabled;
    return ESP_OK;
}
#endif

typedef enum {
    STREAM_PANEL,
    STREAM_MAIN,
} stream_side_t;

typedef struct {
    uart_port_t uart;
    gpio_num_t rx_gpio;
    gpio_num_t tx_gpio;
    stream_side_t side;
    const char *name;
} link_side_t;

typedef struct {
    uint8_t bytes[AC_PROTOCOL_MAX_FRAME_SIZE];
    size_t length;
    size_t expected;
    int64_t last_byte_us;
    bool synchronized;
#if KLIMA_HYBRID_BRIDGE && !KLIMA_SYNCED_TX_PROBE
    /* The main board accepts one panel reply in a narrow response window.
     * Buffering the complete 15-byte frame adds about 16 ms at 9600 baud and
     * is too late.  Prepare a replacement from the last valid panel state at
     * frame start, then emit every received byte immediately. */
    uint8_t hybrid_overlay_frame[AC_PANEL_FRAME_SIZE];
    bool hybrid_overlay_valid;
    bool hybrid_overlay_burst_sent;
#endif
} frame_stream_t;

typedef struct {
    bool active;
    bool event_pending;
    bool confirmation_pending;
    bool takeover_pending;
    bool takeover_ready;
    uint32_t sequence;
    int64_t queued_time_us;
    ac_control_request_t request;
} control_overlay_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_boot_force_off_until_us;
static bridge_ac_state_t s_state;
static control_overlay_t s_overlay;
#if KLIMA_NTS0104_PREFLIGHT
static bool s_hardware_ready;
#else
static bool s_hardware_ready = true;
#endif
static int64_t s_main_frame_time_us;
static int64_t s_panel_frame_time_us;
static int64_t s_physical_panel_frame_time_us;
/* Updated for every byte observed on the panel source.  A command takeover
 * is allowed only after this line has been idle for the complete UART-frame
 * gap, so ETM can never be disabled halfway through a physical frame. */
static volatile int64_t s_panel_last_byte_us;
#if KLIMA_TAP_INJECT
#define TAP_ECHO_WINDOW_US 50000LL
static uint8_t s_tap_last_injected[AC_PANEL_FRAME_SIZE];
static int64_t s_tap_last_injected_time_us;
static bool s_tap_echo_pending;
#endif

#if KLIMA_BRIDGE_ACTIVE
static volatile uint32_t s_panel_tx_pad_edges;
static volatile uint32_t s_main_tx_pad_edges;
#if KLIMA_A4_B4_DIAGNOSTIC
static volatile uint32_t s_a4_monitor_edges;
static volatile uint32_t s_b4_monitor_edges;
static volatile uint32_t s_b4_monitor_bytes;
static volatile uint32_t s_b4_monitor_frames;
static volatile uint32_t s_b4_monitor_checksum_errors;
static uint8_t s_b4_monitor_raw[AC_PANEL_FRAME_SIZE];
static volatile bool s_b4_capture_active;
static volatile int64_t s_b4_capture_start_us;
static TaskHandle_t s_b4_monitor_task_handle;
static volatile int64_t s_b4_low_start_us;
static volatile uint32_t s_b4_low_last_us;
static volatile uint32_t s_b4_low_min_us;
static volatile uint32_t s_b4_low_max_us;
static volatile uint32_t s_b4_capture_lateness_us;
#endif
#if KLIMA_CONVERTER_LOOPBACK_PROBE
static volatile uint32_t s_converter_loopback_edges;
#endif

static void IRAM_ATTR count_active_tx_edge_isr(void *arg)
{
    volatile uint32_t *counter = (volatile uint32_t *)arg;
    ++(*counter);
}

static esp_err_t monitor_active_tx_pad(gpio_num_t gpio, volatile uint32_t *counter)
{
    gpio_ll_input_enable(&GPIO, gpio);
    ESP_RETURN_ON_ERROR(
        gpio_set_intr_type(gpio, GPIO_INTR_ANYEDGE),
        "bridge",
        "configure TX pad edge monitor");
    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(gpio, count_active_tx_edge_isr, (void *)counter),
        "bridge",
        "install TX pad edge monitor");
    return ESP_OK;
}
#endif

#if KLIMA_SYNCED_TX_PROBE
static rmt_channel_handle_t s_sync_tx_channel;
static rmt_encoder_handle_t s_sync_tx_encoder;
static TaskHandle_t s_sync_tx_task_handle;
static esp_timer_handle_t s_sync_tx_timer;
static volatile bool s_sync_tx_active;
static volatile bool s_sync_tx_ready;
static bool s_sync_tx_event_pending;
static uint32_t s_sync_tx_sequence;
static volatile uint32_t s_sync_tx_attempt;
static rmt_symbol_word_t s_sync_event_symbols[SYNC_TX_SYMBOLS];
static rmt_symbol_word_t s_sync_stable_symbols[SYNC_TX_SYMBOLS];
static rmt_symbol_word_t s_sync_passthrough_symbols[SYNC_TX_SYMBOLS];

static uint16_t sync_uart_bit_duration(size_t bit_index)
{
    const uint64_t before =
        ((uint64_t)bit_index * SYNC_TX_RESOLUTION_HZ + BRIDGE_BAUD_RATE / 2U) /
        BRIDGE_BAUD_RATE;
    const uint64_t after =
        ((uint64_t)(bit_index + 1U) * SYNC_TX_RESOLUTION_HZ +
         BRIDGE_BAUD_RATE / 2U) /
        BRIDGE_BAUD_RATE;
    return (uint16_t)(after - before);
}

static void sync_encode_uart_frame(
    const uint8_t frame[AC_PANEL_FRAME_SIZE],
    rmt_symbol_word_t symbols[SYNC_TX_SYMBOLS])
{
    uint8_t levels[SYNC_TX_UART_BITS];
    size_t bit = 0U;
    for (size_t byte_index = 0U; byte_index < AC_PANEL_FRAME_SIZE; ++byte_index) {
        levels[bit++] = 0U;
        for (unsigned data_bit = 0U; data_bit < 8U; ++data_bit) {
            levels[bit++] = (frame[byte_index] >> data_bit) & 1U;
        }
        levels[bit++] = 1U;
    }

    for (size_t symbol = 0U; symbol < SYNC_TX_SYMBOLS; ++symbol) {
        const size_t first_bit = symbol * 2U;
        symbols[symbol] = (rmt_symbol_word_t){
            .duration0 = sync_uart_bit_duration(first_bit),
            .level0 = levels[first_bit],
            .duration1 = sync_uart_bit_duration(first_bit + 1U),
            .level1 = levels[first_bit + 1U],
        };
    }
}

static bool sync_prepare_control_frames_locked(void)
{
    uint8_t event_frame[AC_PANEL_FRAME_SIZE];
    uint8_t stable_frame[AC_PANEL_FRAME_SIZE];
    memcpy(event_frame, s_state.panel_raw, sizeof(event_frame));
    memcpy(stable_frame, s_state.panel_raw, sizeof(stable_frame));
    if (!ac_protocol_apply_control(event_frame, &s_overlay.request, true) ||
        !ac_protocol_apply_control(stable_frame, &s_overlay.request, false)) {
        return false;
    }
    sync_encode_uart_frame(event_frame, s_sync_event_symbols);
    sync_encode_uart_frame(stable_frame, s_sync_stable_symbols);
    s_sync_tx_event_pending = true;
    s_sync_tx_sequence = s_overlay.sequence;
    __atomic_store_n(&s_sync_tx_attempt, 0U, __ATOMIC_RELEASE);
    s_sync_tx_active = true;
    return true;
}

static void sync_update_passthrough_frame(
    const uint8_t frame[AC_PANEL_FRAME_SIZE])
{
    rmt_symbol_word_t symbols[SYNC_TX_SYMBOLS];
    sync_encode_uart_frame(frame, symbols);
    taskENTER_CRITICAL(&s_lock);
    memcpy(s_sync_passthrough_symbols, symbols, sizeof(symbols));
    s_sync_tx_ready = true;
    taskEXIT_CRITICAL(&s_lock);
}

static void sync_tx_timer_callback(void *arg)
{
    (void)arg;
    if (s_sync_tx_ready && s_sync_tx_task_handle) {
        xTaskNotifyGive(s_sync_tx_task_handle);
    }
}

static void sync_tx_task(void *arg)
{
    (void)arg;
    const rmt_transmit_config_t config = {
        .loop_count = 0,
        .flags.eot_level = 1,
        .flags.queue_nonblocking = 1,
    };
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        rmt_symbol_word_t symbols[SYNC_TX_SYMBOLS];
        uint32_t sequence = 0U;
        bool event = false;
        bool ready = false;
        taskENTER_CRITICAL(&s_lock);
        if (s_sync_tx_ready) {
            ready = true;
            event = s_sync_tx_event_pending;
            if (s_sync_tx_active) {
                sequence = s_sync_tx_sequence;
                memcpy(
                    symbols,
                    event ? s_sync_event_symbols : s_sync_stable_symbols,
                    sizeof(symbols));
            } else {
                event = false;
                memcpy(symbols, s_sync_passthrough_symbols, sizeof(symbols));
            }
        }
        taskEXIT_CRITICAL(&s_lock);
        if (!ready) {
            continue;
        }
        const esp_err_t result = rmt_transmit(
            s_sync_tx_channel,
            s_sync_tx_encoder,
            symbols,
            sizeof(symbols),
            &config);
        if (result != ESP_OK) {
            telemetry_publishf(
                "SYNC_TX,event=transmit_error,sequence=%" PRIu32 ",error=%s",
                sequence,
                esp_err_to_name(result));
        } else if (event) {
            telemetry_publishf(
                "SYNC_TX,event=event_started,sequence=%" PRIu32,
                sequence);
        }
        if (result == ESP_OK) {
            const esp_err_t wait_result =
                rmt_tx_wait_all_done(s_sync_tx_channel, 100);
            if (wait_result != ESP_OK) {
                telemetry_publishf(
                    "SYNC_TX,event=wait_error,error=%s",
                    esp_err_to_name(wait_result));
            }
        }
    }
}

static esp_err_t start_synced_tx_output(void)
{
    const rmt_tx_channel_config_t channel_config = {
        .gpio_num = MAIN_TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = SYNC_TX_RESOLUTION_HZ,
        .mem_block_symbols = 96,
        .trans_queue_depth = 2,
        .intr_priority = 3,
    };
    ESP_RETURN_ON_ERROR(
        rmt_new_tx_channel(&channel_config, &s_sync_tx_channel),
        "bridge",
        "create synchronized RMT TX");
    const rmt_copy_encoder_config_t encoder_config = {};
    ESP_RETURN_ON_ERROR(
        rmt_new_copy_encoder(&encoder_config, &s_sync_tx_encoder),
        "bridge",
        "create synchronized RMT encoder");
    ESP_RETURN_ON_ERROR(
        rmt_enable(s_sync_tx_channel),
        "bridge",
        "enable synchronized RMT TX");
    ESP_RETURN_ON_ERROR(
        gpio_set_drive_capability(MAIN_TX_GPIO, GPIO_DRIVE_CAP_3),
        "bridge",
        "set synchronized TX drive strength");
    telemetry_publishf(
        "SYNC_TX,event=started,gpio=%d,resolution_hz=%u,baud=%u",
        MAIN_TX_GPIO,
        SYNC_TX_RESOLUTION_HZ,
        BRIDGE_BAUD_RATE);
    return ESP_OK;
}
#endif

#if (KLIMA_MAIN_EDGE_FORWARD && !KLIMA_MAIN_HARDWARE_FORWARD) || \
    KLIMA_BRIDGE_MIRROR_PROBE || KLIMA_WAKE7_PROBE
typedef struct {
    gpio_num_t source;
    gpio_num_t destination;
    volatile uint32_t *counter;
} edge_mirror_t;

static volatile uint32_t s_probe_main_edges;
static volatile uint32_t s_probe_panel_gpio5_edges;
static volatile uint32_t s_probe_alternate_panel_edges;
static edge_mirror_t s_main_to_panel_mirror = {
    .source = PROBE_MAIN_RX_GPIO,
    .destination = PROBE_MAIN_TO_PANEL_GPIO,
    .counter = &s_probe_main_edges,
};

static void IRAM_ATTR mirror_edge_isr(void *arg)
{
    const edge_mirror_t *mirror = (const edge_mirror_t *)arg;
    gpio_set_level(mirror->destination, gpio_get_level(mirror->source));
    ++(*mirror->counter);
}

#if KLIMA_BRIDGE_MIRROR_PROBE || KLIMA_WAKE7_PROBE
static void IRAM_ATTR count_edge_isr(void *arg)
{
    volatile uint32_t *counter = (volatile uint32_t *)arg;
    ++(*counter);
}
#endif

#endif

#if KLIMA_ALL_INPUTS_PROBE
static volatile uint32_t s_all_input_edges[4];
static const gpio_num_t k_all_input_gpios[4] = {
    GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7,
};

static void IRAM_ATTR count_all_input_edge_isr(void *arg)
{
    volatile uint32_t *counter = (volatile uint32_t *)arg;
    ++(*counter);
}
#endif

#if !KLIMA_BRIDGE_ACTIVE && !KLIMA_ALL_INPUTS_PROBE && \
    !KLIMA_HARDWARE_BYPASS_PROBE && !KLIMA_BRIDGE_MIRROR_PROBE && \
    !KLIMA_WAKE7_PROBE && !KLIMA_PANEL_BENCH_EMULATOR
/* The installed PASSIVE profile keeps GPIO6/GPIO7 input-only. Count their
 * transitions as auxiliary continuity diagnostics without changing either
 * factory UART line or enabling a transmit path. */
static volatile uint32_t s_passive_aux_edges[2];

static void IRAM_ATTR count_passive_aux_edge_isr(void *arg)
{
    volatile uint32_t *counter = (volatile uint32_t *)arg;
    ++(*counter);
}
#endif

#if KLIMA_PANEL_BENCH_EMULATOR
static void panel_bench_emulator_task(void *arg)
{
    (void)arg;
    uint8_t main_reply[AC_MAIN_FRAME_SIZE] = {
        0x54, 0x44, 0x12, 0x11, 0x31, 0x00, 0x12, 0x00,
        0x00, 0x00, 0x00, 0x2C, 0x2C, 0x00, 0x40, 0x0A,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    ac_protocol_finalize(main_reply, sizeof(main_reply));

    for (;;) {
        bool ready;
        bool panel_valid;
        uint8_t panel[AC_PANEL_FRAME_SIZE];
        taskENTER_CRITICAL(&s_lock);
        ready = s_hardware_ready;
        panel_valid = s_state.panel_valid;
        memcpy(panel, s_state.panel_raw, sizeof(panel));
        taskEXIT_CRITICAL(&s_lock);
        if (!ready) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (panel_valid && ac_protocol_checksum_valid(panel, sizeof(panel))) {
            /* Acknowledge the detached panel exactly as the real main board
             * does: copy its event/state, normalize FAN 0x22 -> 0x21 and
             * return matching power, setpoint and display-temperature data. */
            main_reply[3] = panel[3];
            main_reply[4] = ac_protocol_panel_to_main_mode_fan(panel[4]);
            main_reply[5] = panel[5];
            main_reply[6] = panel[6];
            main_reply[8] = (panel[5] & 0x80U) != 0U ? 0x02U : 0x00U;
            main_reply[14] = panel[9];
            ac_protocol_finalize(main_reply, sizeof(main_reply));
        }
        const int written = uart_write_bytes(PANEL_UART, main_reply, sizeof(main_reply));
        if (written != (int)sizeof(main_reply)) {
            telemetry_publishf(
                "PANEL_BENCH,event=short_write,expected=%u,written=%d",
                (unsigned)sizeof(main_reply), written);
        }
        vTaskDelay(pdMS_TO_TICKS(BRIDGE_VIRTUAL_PANEL_PERIOD_MS));
    }
}
#endif

static const link_side_t k_panel_side = {
    .uart = PANEL_UART,
    .rx_gpio = PANEL_RX_GPIO,
    .tx_gpio = PANEL_TX_GPIO,
    .side = STREAM_PANEL,
    .name = "panel_to_main",
};

static const link_side_t k_main_side = {
    .uart = MAIN_UART,
    .rx_gpio = MAIN_RX_GPIO,
    .tx_gpio = MAIN_TX_GPIO,
    .side = STREAM_MAIN,
    .name = "main_to_panel",
};

static void format_hex(char *output, size_t output_size, const uint8_t *data, size_t length)
{
    size_t offset = 0U;
    for (size_t i = 0; i < length && offset + 2U < output_size; ++i) {
        const int written = snprintf(output + offset, output_size - offset, "%02X", data[i]);
        if (written <= 0) {
            break;
        }
        offset += (size_t)written;
    }
}

static void publish_frame(
    const char *direction,
    const uint8_t *frame,
    size_t length,
    const char *kind)
{
    capture_direction_t capture_direction = CAPTURE_DIRECTION_NONE;
    capture_kind_t capture_kind = CAPTURE_KIND_ERROR;
    if (strcmp(direction, "panel_to_main") == 0) {
        capture_direction = CAPTURE_DIRECTION_PANEL_TO_MAIN;
    } else if (strcmp(direction, "main_to_panel") == 0) {
        capture_direction = CAPTURE_DIRECTION_MAIN_TO_PANEL;
    }
    if (strcmp(kind, "initial") == 0) {
        capture_kind = CAPTURE_KIND_INITIAL;
    } else if (strcmp(kind, "state_change") == 0) {
        capture_kind = CAPTURE_KIND_STATE_CHANGE;
    } else if (strcmp(kind, "event") == 0) {
        capture_kind = CAPTURE_KIND_EVENT;
    } else if (strcmp(kind, "wifi_command") == 0) {
        capture_kind = CAPTURE_KIND_WIFI_COMMAND;
    }
    capture_store_record_frame(capture_direction, capture_kind, frame, length);

    char hex[AC_PROTOCOL_MAX_FRAME_SIZE * 2U + 1U] = {0};
    format_hex(hex, sizeof(hex), frame, length);
    telemetry_publishf(
        "FRAME,t_us=%" PRId64 ",direction=%s,kind=%s,len=%u,hex=%s",
        esp_timer_get_time(),
        direction,
        kind,
        (unsigned)length,
        hex);
}

static esp_err_t configure_uart(
    uart_port_t uart,
    gpio_num_t tx_gpio,
    gpio_num_t rx_gpio,
    bool with_tx)
{
    const uart_config_t config = {
        .baud_rate = BRIDGE_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t result = uart_driver_delete(uart);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    ESP_RETURN_ON_ERROR(
        uart_driver_install(
            uart,
            BRIDGE_RX_BUFFER_SIZE,
            with_tx ? BRIDGE_TX_BUFFER_SIZE : 0,
            0,
            NULL,
            0),
        "bridge",
        "install UART");
    ESP_RETURN_ON_ERROR(uart_param_config(uart, &config), "bridge", "configure UART");
    ESP_RETURN_ON_ERROR(
        uart_set_pin(
            uart,
            with_tx ? tx_gpio : (gpio_num_t)UART_PIN_NO_CHANGE,
            rx_gpio,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE),
        "bridge",
        "route UART pins");
    ESP_RETURN_ON_ERROR(uart_set_rx_full_threshold(uart, 1), "bridge", "RX threshold");
    ESP_RETURN_ON_ERROR(uart_set_rx_timeout(uart, 2), "bridge", "RX timeout");
    ESP_RETURN_ON_ERROR(gpio_set_pull_mode(rx_gpio, GPIO_PULLUP_ONLY), "bridge", "RX pull-up");
    return ESP_OK;
}

#if KLIMA_A4_B4_DIAGNOSTIC
static void IRAM_ATTR b4_monitor_edge_isr(void *arg)
{
    (void)arg;
    ++s_b4_monitor_edges;
    const int level = gpio_get_level(B4_MONITOR_GPIO);
    const int64_t now = esp_timer_get_time();
    if (level != 0) {
        if (s_b4_low_start_us > 0) {
            const uint32_t width = (uint32_t)(now - s_b4_low_start_us);
            s_b4_low_last_us = width;
            if (s_b4_low_min_us == 0U || width < s_b4_low_min_us) {
                s_b4_low_min_us = width;
            }
            if (width > s_b4_low_max_us) {
                s_b4_low_max_us = width;
            }
            s_b4_low_start_us = 0;
        }
        return;
    }
    s_b4_low_start_us = now;
    if (s_b4_capture_active || !s_b4_monitor_task_handle) {
        return;
    }
    s_b4_capture_active = true;
    s_b4_capture_start_us = now;
    BaseType_t high_task_wakeup = pdFALSE;
    vTaskNotifyGiveFromISR(s_b4_monitor_task_handle, &high_task_wakeup);
    if (high_task_wakeup == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void b4_wait_until(int64_t target_us)
{
    for (;;) {
        const int64_t remaining = target_us - esp_timer_get_time();
        if (remaining <= 0) {
            return;
        }
        if (remaining > 20) {
            esp_rom_delay_us((uint32_t)(remaining - 10));
        }
    }
}

static void b4_monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int64_t start = s_b4_capture_start_us;
        const int64_t task_started = esp_timer_get_time();
        s_b4_capture_lateness_us =
            task_started > start ? (uint32_t)(task_started - start) : 0U;
        uint8_t frame[AC_PANEL_FRAME_SIZE] = {0};
        bool stop_bits_valid = true;
        for (size_t byte = 0U; byte < AC_PANEL_FRAME_SIZE; ++byte) {
            for (unsigned bit = 0U; bit < 8U; ++bit) {
                /* Absolute center sampling avoids accumulating rounding error:
                 * bit period = 625/6 us at 9600 baud. */
                const uint32_t numerator =
                    (uint32_t)(byte * 12500U) + (3U + 2U * bit) * 625U;
                b4_wait_until(start + (int64_t)((numerator + 6U) / 12U));
                frame[byte] |= (uint8_t)(gpio_get_level(B4_MONITOR_GPIO) << bit);
            }
            const uint32_t stop_numerator =
                (uint32_t)(byte * 12500U) + 19U * 625U;
            b4_wait_until(start + (int64_t)((stop_numerator + 6U) / 12U));
            if (gpio_get_level(B4_MONITOR_GPIO) == 0) {
                stop_bits_valid = false;
            }
        }
        __atomic_add_fetch(
            &s_b4_monitor_bytes,
            AC_PANEL_FRAME_SIZE,
            __ATOMIC_RELAXED);
        taskENTER_CRITICAL(&s_lock);
        memcpy(s_b4_monitor_raw, frame, sizeof(frame));
        taskEXIT_CRITICAL(&s_lock);
        if (stop_bits_valid && ac_protocol_checksum_valid(frame, sizeof(frame))) {
            taskENTER_CRITICAL(&s_lock);
            ++s_b4_monitor_frames;
            taskEXIT_CRITICAL(&s_lock);
        } else {
            ++s_b4_monitor_checksum_errors;
        }
        s_b4_capture_active = false;
    }
}
#endif

static void update_panel_state(
    const uint8_t frame[AC_PANEL_FRAME_SIZE],
    const uint8_t forwarded[AC_PANEL_FRAME_SIZE],
    bool injected,
    bool emulated)
{
    ac_panel_state_t parsed;
    if (!ac_protocol_parse_panel(frame, &parsed)) {
        taskENTER_CRITICAL(&s_lock);
        ++s_state.checksum_errors;
        taskEXIT_CRITICAL(&s_lock);
        return;
    }

    const int64_t now = esp_timer_get_time();
    bool first = false;
    bool changed = false;
    taskENTER_CRITICAL(&s_lock);
    first = !s_state.panel_valid;
    changed = first || memcmp(s_state.panel_raw, frame, AC_PANEL_FRAME_SIZE) != 0;
    memcpy(s_state.panel_raw, frame, AC_PANEL_FRAME_SIZE);
    memcpy(s_state.forwarded_panel_raw, forwarded, AC_PANEL_FRAME_SIZE);
    s_state.panel_valid = true;
    s_state.panel_emulated = emulated;
    s_state.panel_event = parsed.event;
    s_state.setpoint_c = parsed.setpoint_c;
    ++s_state.panel_frames;
    if (injected) {
        ++s_state.injected_frames;
    }
    s_panel_frame_time_us = now;
    taskEXIT_CRITICAL(&s_lock);
    if (changed) {
        publish_frame(
            "panel_to_main",
            frame,
            AC_PANEL_FRAME_SIZE,
            parsed.event ? "event" : (first ? "initial" : "state_change"));
    }
}

static void update_main_state(const uint8_t frame[AC_MAIN_FRAME_SIZE])
{
    ac_main_state_t parsed;
    if (!ac_protocol_parse_main(frame, &parsed)) {
        taskENTER_CRITICAL(&s_lock);
        ++s_state.checksum_errors;
        taskEXIT_CRITICAL(&s_lock);
        return;
    }

    const int64_t now = esp_timer_get_time();
    bool first = false;
    bool changed = false;
    bool restore_panel_mirror = false;
    taskENTER_CRITICAL(&s_lock);
    first = !s_state.main_valid;
    changed = first || memcmp(s_state.main_raw, frame, AC_MAIN_FRAME_SIZE) != 0;
    memcpy(s_state.main_raw, frame, AC_MAIN_FRAME_SIZE);
    s_state.main_valid = true;
    s_state.power = parsed.power;
    s_state.feature_flags = parsed.feature_flags;
    s_state.main_display_temperature_c = parsed.display_temperature_c;
    s_state.main_display_temperature_encoded = parsed.display_temperature_encoded;
    if (parsed.mode == AC_MODE_COOL) {
        s_state.setpoint_c = parsed.display_temperature_c;
    }
    s_state.mode_fan_code = parsed.mode_fan_code;
    s_state.mode = parsed.mode;
    s_state.fan = parsed.fan;
    s_state.operation_code = parsed.operation_code;
    s_state.sensor_1_raw = parsed.sensor_1_raw;
    s_state.sensor_2_raw = parsed.sensor_2_raw;
    ++s_state.main_frames;
    s_main_frame_time_us = now;

    if (s_overlay.confirmation_pending) {
        bool confirmed = true;
        const ac_control_request_t *request = &s_overlay.request;
        if ((request->mask & AC_CONTROL_POWER) != 0U) {
            confirmed = confirmed && parsed.power == request->power;
        }
        if ((request->mask & AC_CONTROL_SETPOINT) != 0U) {
            confirmed = confirmed && parsed.mode == AC_MODE_COOL &&
                parsed.display_temperature_c == request->setpoint_c;
        }
        if ((request->mask & AC_CONTROL_MODE) != 0U) {
            confirmed = confirmed && parsed.mode == request->mode;
        }
        if ((request->mask & AC_CONTROL_FAN) != 0U && parsed.mode == AC_MODE_COOL) {
            confirmed = confirmed && parsed.fan == request->fan;
        }
        if ((request->mask & AC_CONTROL_RAW_MODE_FAN) != 0U) {
            /* FAN is normalized by the main board from panel 0x22 to main 0x21. */
            const uint8_t expected = ac_protocol_panel_to_main_mode_fan(
                request->raw_mode_fan_code);
            confirmed = confirmed && parsed.mode_fan_code == expected;
        }
        if ((request->mask & AC_CONTROL_QUIET) != 0U) {
            confirmed = confirmed &&
                ((parsed.feature_flags & AC_FEATURE_QUIET) != 0U) == request->quiet;
        }
        if ((request->mask & AC_CONTROL_UNITS_FAHRENHEIT) != 0U) {
            confirmed = confirmed &&
                ((parsed.feature_flags & AC_FEATURE_UNITS_FAHRENHEIT) != 0U) ==
                    request->units_fahrenheit;
        }
        if ((request->mask & AC_CONTROL_TIMER) != 0U) {
            confirmed = confirmed &&
                ((parsed.feature_flags & AC_FEATURE_TIMER) != 0U) == request->timer;
        }
        if (confirmed) {
            /* A response-confirmed command is complete. Keeping the overlay
             * active would merge its old SETPOINT/FAN bits into the next
             * unrelated command (for example Cool -> Fan), producing a real
             * state change followed by a false timeout. From this point the
             * main board is authoritative again. */
            s_overlay.active = false;
            s_overlay.event_pending = false;
            s_overlay.confirmation_pending = false;
            s_overlay.takeover_pending = false;
            s_overlay.takeover_ready = false;
            s_overlay.queued_time_us = 0;
            s_overlay.request = (ac_control_request_t){0};
            s_state.override_active = false;
            s_state.command_pending = false;
            s_state.command_status = BRIDGE_COMMAND_CONFIRMED;
            restore_panel_mirror = true;
#if KLIMA_SYNCED_TX_PROBE
            s_sync_tx_active = false;
#endif
        }
    }
    taskEXIT_CRITICAL(&s_lock);
#if KLIMA_SYNCED_TX_PROBE
    if (s_sync_tx_timer) {
        uint64_t launch_delay_us = SYNC_TX_AFTER_MAIN_US;
        uint32_t launch_attempt = 0U;
        uint32_t launch_sequence = 0U;
        if (s_sync_tx_active) {
            launch_attempt = __atomic_fetch_add(
                &s_sync_tx_attempt, 1U, __ATOMIC_ACQ_REL);
            launch_delay_us = SYNC_TX_SCAN_START_US +
                (uint64_t)(launch_attempt % SYNC_TX_SCAN_SLOTS) * SYNC_TX_SCAN_STEP_US;
            launch_sequence = s_sync_tx_sequence;
        }
        const esp_err_t stop_result = esp_timer_stop(s_sync_tx_timer);
        if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) {
            telemetry_publishf(
                "SYNC_TX,event=schedule_stop_error,error=%s",
                esp_err_to_name(stop_result));
        }
        const esp_err_t start_result =
            esp_timer_start_once(s_sync_tx_timer, launch_delay_us);
        if (start_result != ESP_OK) {
            telemetry_publishf(
                "SYNC_TX,event=schedule_start_error,error=%s",
                esp_err_to_name(start_result));
        } else if (s_sync_tx_active) {
            telemetry_publishf(
                "SYNC_TX,event=scan_scheduled,sequence=%" PRIu32
                ",attempt=%" PRIu32 ",delay_us=%u",
                launch_sequence,
                launch_attempt + 1U,
                (unsigned)launch_delay_us);
        }
    }
#endif
    if (restore_panel_mirror) {
        const esp_err_t result =
            set_panel_to_main_hardware_mirror_enabled(true);
        if (result != ESP_OK) {
            telemetry_publishf(
                "CONTROL,event=etm_restore_failed,error=%s",
                esp_err_to_name(result));
        }
    }
    if (changed) {
        publish_frame(
            "main_to_panel",
            frame,
            AC_MAIN_FRAME_SIZE,
            parsed.event ? "event" : (first ? "initial" : "state_change"));
    }
}

static bool panel_state_matches_request(
    const ac_panel_state_t *state,
    const ac_control_request_t *request)
{
    bool matches = true;
    if ((request->mask & AC_CONTROL_POWER) != 0U) {
        matches = matches && state->power == request->power;
    }
    if ((request->mask & AC_CONTROL_SETPOINT) != 0U) {
        matches = matches && state->setpoint_c == request->setpoint_c;
    }
    if ((request->mask & AC_CONTROL_MODE) != 0U) {
        matches = matches && state->mode == request->mode;
    }
    if ((request->mask & AC_CONTROL_FAN) != 0U && state->mode == AC_MODE_COOL) {
        matches = matches && state->fan == request->fan;
    }
    if ((request->mask & AC_CONTROL_RAW_MODE_FAN) != 0U) {
        matches = matches && state->mode_fan_code == request->raw_mode_fan_code;
    }
    if ((request->mask & AC_CONTROL_QUIET) != 0U) {
        matches = matches &&
            ((state->feature_flags & AC_FEATURE_QUIET) != 0U) == request->quiet;
    }
    if ((request->mask & AC_CONTROL_UNITS_FAHRENHEIT) != 0U) {
        matches = matches &&
            ((state->feature_flags & AC_FEATURE_UNITS_FAHRENHEIT) != 0U) ==
                request->units_fahrenheit;
    }
    if ((request->mask & AC_CONTROL_TIMER) != 0U) {
        matches = matches &&
            ((state->feature_flags & AC_FEATURE_TIMER) != 0U) == request->timer;
    }
    return matches;
}

#if KLIMA_BRIDGE_ACTIVE
static void forward_bytes(uart_port_t destination, const uint8_t *data, size_t length)
{
    const int written = uart_write_bytes(destination, data, length);
    if (written != (int)length) {
        telemetry_publishf(
            "BRIDGE_ERROR,event=short_write,destination=%d,expected=%u,written=%d",
            destination,
            (unsigned)length,
            written);
    }
}

#if KLIMA_HYBRID_BRIDGE && !KLIMA_SYNCED_TX_PROBE
static void hybrid_prepare_panel_frame(frame_stream_t *stream)
{
    control_overlay_t overlay;
    bool panel_valid;

    taskENTER_CRITICAL(&s_lock);
    overlay = s_overlay;
    panel_valid = s_state.panel_valid;
    if (panel_valid) {
        memcpy(
            stream->hybrid_overlay_frame,
            s_state.panel_raw,
            AC_PANEL_FRAME_SIZE);
    }
    taskEXIT_CRITICAL(&s_lock);

    stream->hybrid_overlay_valid = panel_valid && overlay.active &&
        overlay.takeover_ready &&
        ac_protocol_apply_control(
            stream->hybrid_overlay_frame,
            &overlay.request,
            overlay.event_pending);
    stream->hybrid_overlay_burst_sent = false;
}

static void hybrid_forward_panel_byte(
    frame_stream_t *stream,
    size_t index,
    uint8_t received)
{
    if (index == 0U) {
        hybrid_prepare_panel_frame(stream);
    }

    const uint8_t outgoing =
        stream->hybrid_overlay_valid && index < AC_PANEL_FRAME_SIZE
        ? stream->hybrid_overlay_frame[index]
        : received;
#if KLIMA_PANEL_HARDWARE_FORWARD
    /* ETM already carries the unmodified panel source while idle.  Sending
     * the same byte through UART here would create duplicate start bits.  The
     * CPU path is enabled only for the short command-takeover interval. */
    if (!s_panel_to_main_hardware_mirror_enabled) {
        /* The mirror is disabled only at an inter-frame idle boundary. Once
         * we own the line, emit the complete replacement as one contiguous
         * UART transfer and ignore the corresponding source frame. Byte 3
         * cannot be used as a veto: this panel legitimately keeps 0x12 in
         * identical periodic frames. A genuine physical state change is
         * detected after the complete payload and restores ETM for the next
         * repeated panel frame. */
        if (stream->hybrid_overlay_burst_sent) {
            return;
        }
        if (index == 0U && stream->hybrid_overlay_valid) {
            forward_bytes(
                MAIN_UART,
                stream->hybrid_overlay_frame,
                AC_PANEL_FRAME_SIZE);
            stream->hybrid_overlay_burst_sent = true;
            stream->hybrid_overlay_valid = false;
            return;
        }
        forward_bytes(MAIN_UART, &outgoing, 1U);
    }
#else
    /* In the canonical UART-regenerated profile the destination already
     * belongs to MAIN_UART. During an overlay, queue the entire command at
     * the first source byte and suppress the rest of that source frame. This
     * avoids per-byte scheduler/queue jitter on the only frame that must be
     * interpreted as an atomic panel command. */
    if (stream->hybrid_overlay_burst_sent) {
        return;
    }
    if (index == 0U && stream->hybrid_overlay_valid) {
        forward_bytes(
            MAIN_UART,
            stream->hybrid_overlay_frame,
            AC_PANEL_FRAME_SIZE);
        stream->hybrid_overlay_burst_sent = true;
        stream->hybrid_overlay_valid = false;
        return;
    }
    forward_bytes(MAIN_UART, &outgoing, 1U);
#endif
}
#endif

#if KLIMA_TAP_INJECT
static void tap_remember_injection(const uint8_t *frame)
{
    taskENTER_CRITICAL(&s_lock);
    memcpy(s_tap_last_injected, frame, AC_PANEL_FRAME_SIZE);
    s_tap_last_injected_time_us = esp_timer_get_time();
    s_tap_echo_pending = true;
    taskEXIT_CRITICAL(&s_lock);
}

static bool tap_consume_injected_echo(const uint8_t *frame)
{
    const int64_t now = esp_timer_get_time();
    bool echo = false;
    taskENTER_CRITICAL(&s_lock);
    if (s_tap_echo_pending &&
        now - s_tap_last_injected_time_us <= TAP_ECHO_WINDOW_US &&
        memcmp(s_tap_last_injected, frame, AC_PANEL_FRAME_SIZE) == 0) {
        s_tap_echo_pending = false;
        echo = true;
    } else if (s_tap_echo_pending &&
               now - s_tap_last_injected_time_us > TAP_ECHO_WINDOW_US) {
        s_tap_echo_pending = false;
    }
    taskEXIT_CRITICAL(&s_lock);
    return echo;
}

static void tap_inject_frame(const uint8_t *frame)
{
    /* MAIN_TX_GPIO and GPIO7 are tied by the legacy factory-bypass probe.
     * Remember the outgoing frame before writing it so UART0 can discard the
     * local echo. */
    tap_remember_injection(frame);
    forward_bytes(MAIN_UART, frame, AC_PANEL_FRAME_SIZE);
}
#endif
#endif

static void process_complete_frame(
    const link_side_t *side,
    const uint8_t *frame,
    size_t length)
{
    if (!ac_protocol_checksum_valid(frame, length)) {
        taskENTER_CRITICAL(&s_lock);
        ++s_state.checksum_errors;
        taskEXIT_CRITICAL(&s_lock);
#if KLIMA_BRIDGE_ACTIVE && !KLIMA_TAP_INJECT && !KLIMA_HYBRID_BRIDGE
        if (side->side == STREAM_PANEL) {
            forward_bytes(MAIN_UART, frame, length);
        }
#endif
        publish_frame(side->name, frame, length, "checksum_error");
        return;
    }

    if (side->side == STREAM_MAIN && length == AC_MAIN_FRAME_SIZE) {
#if KLIMA_BRIDGE_ACTIVE && !KLIMA_MAIN_EDGE_FORWARD && !KLIMA_TAP_INJECT
        /* Regenerate the complete frame in one UART transfer. This keeps the
         * wire bytes contiguous and avoids an ISR on every UART edge. */
        uint8_t panel_status[AC_MAIN_FRAME_SIZE];
        memcpy(panel_status, frame, sizeof(panel_status));
#if KLIMA_BOOT_FORCE_OFF_DIAG
        if (esp_timer_get_time() < s_boot_force_off_until_us) {
            static const uint8_t captured_off[AC_MAIN_FRAME_SIZE] = {
                0x54, 0x44, 0x12, 0x11, 0x31, 0x00, 0x12, 0x00,
                0x00, 0x00, 0x00, 0x2D, 0x2D, 0x00, 0x40, 0x0A,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x6A,
            };
            memcpy(panel_status, captured_off, sizeof(panel_status));
            forward_bytes(PANEL_UART, panel_status, sizeof(panel_status));
        } else
#endif
#if KLIMA_PANEL_LOOP_CONTROL
        {
            control_overlay_t panel_overlay;
            taskENTER_CRITICAL(&s_lock);
            panel_overlay = s_overlay;
            taskEXIT_CRITICAL(&s_lock);
            if (panel_overlay.active &&
                ac_protocol_apply_control_main(
                    panel_status,
                    &panel_overlay.request,
                    panel_overlay.event_pending)) {
                forward_bytes(PANEL_UART, panel_status, sizeof(panel_status));
            } else {
                forward_bytes(PANEL_UART, frame, length);
            }
        }
#else
        {
            forward_bytes(PANEL_UART, frame, length);
        }
#endif
#endif
        update_main_state(frame);
        return;
    }

    if (side->side == STREAM_PANEL && length == AC_PANEL_FRAME_SIZE) {
#if KLIMA_TAP_INJECT
        if (tap_consume_injected_echo(frame)) {
            telemetry_publishf("BRIDGE,event=tap_echo_discarded");
            return;
        }
#endif
        uint8_t forwarded[AC_PANEL_FRAME_SIZE];
        memcpy(forwarded, frame, sizeof(forwarded));
        bool injected = false;

#if KLIMA_BRIDGE_ACTIVE
        ac_panel_state_t actual;
        const bool parsed = ac_protocol_parse_panel(frame, &actual);
        control_overlay_t overlay;
        bool cancelled_by_panel = false;
        bool restore_panel_mirror = false;
        uint32_t cancelled_sequence = 0U;
        taskENTER_CRITICAL(&s_lock);
        const bool physical_panel_state_changed =
            s_state.panel_valid &&
            memcmp(
                &frame[4],
                &s_state.panel_raw[4],
                AC_PANEL_FRAME_SIZE - 5U) != 0;
        const bool expected_panel_loop_state = KLIMA_PANEL_LOOP_CONTROL && parsed &&
            s_overlay.active && panel_state_matches_request(&actual, &s_overlay.request);
        if (parsed && actual.event && physical_panel_state_changed &&
            s_overlay.active && !expected_panel_loop_state) {
            /* A real panel/IR state change always wins over Wi-Fi state. Some
             * panel firmware keeps byte 3 at the event value (0x12) across
             * many identical periodic frames, so the marker alone is not a
             * new physical action. Compare the semantic payload bytes 4..13
             * and deliberately exclude both byte 3 and the checksum. */
            cancelled_by_panel = true;
            cancelled_sequence = s_overlay.sequence;
            s_overlay.active = false;
            s_overlay.confirmation_pending = false;
            s_overlay.takeover_pending = false;
            s_overlay.takeover_ready = false;
            s_state.override_active = false;
            s_state.command_pending = false;
            s_state.command_status = BRIDGE_COMMAND_CANCELLED;
            restore_panel_mirror = true;
#if KLIMA_SYNCED_TX_PROBE
            s_sync_tx_active = false;
#endif
        }
        overlay = s_overlay;
        if (overlay.active && overlay.event_pending && !KLIMA_PANEL_LOOP_CONTROL) {
            s_overlay.event_pending = false;
        }
        taskEXIT_CRITICAL(&s_lock);

        if (restore_panel_mirror) {
            const esp_err_t result =
                set_panel_to_main_hardware_mirror_enabled(true);
            if (result != ESP_OK) {
                telemetry_publishf(
                    "CONTROL,event=etm_restore_failed,error=%s",
                    esp_err_to_name(result));
            }
        }

        if (cancelled_by_panel) {
            telemetry_publishf(
                "CONTROL,event=cancelled_by_physical_panel,sequence=%" PRIu32,
                cancelled_sequence);
        }

        if (overlay.active && overlay.takeover_ready && parsed &&
            ac_protocol_apply_control(forwarded, &overlay.request, overlay.event_pending)) {
            injected = true;
#if KLIMA_TAP_INJECT
            tap_inject_frame(forwarded);
#elif !KLIMA_HYBRID_BRIDGE
            forward_bytes(MAIN_UART, forwarded, sizeof(forwarded));
#endif
            if (overlay.event_pending) {
                publish_frame(side->name, forwarded, sizeof(forwarded), "wifi_command");
            }
        } else {
#if !KLIMA_TAP_INJECT && !KLIMA_HYBRID_BRIDGE
            forward_bytes(MAIN_UART, frame, length);
#endif
        }
#endif
        taskENTER_CRITICAL(&s_lock);
        s_physical_panel_frame_time_us = esp_timer_get_time();
        taskEXIT_CRITICAL(&s_lock);
#if KLIMA_SYNCED_TX_PROBE
        if (parsed) {
            sync_update_passthrough_frame(frame);
        }
#endif
        update_panel_state(frame, forwarded, injected, false);
        return;
    }

    taskENTER_CRITICAL(&s_lock);
    ++s_state.framing_errors;
    taskEXIT_CRITICAL(&s_lock);
#if KLIMA_BRIDGE_ACTIVE && !KLIMA_TAP_INJECT && !KLIMA_HYBRID_BRIDGE
    if (side->side == STREAM_PANEL) {
        forward_bytes(MAIN_UART, frame, length);
    }
#endif
    publish_frame(side->name, frame, length, "unexpected_length");
}

static void flush_stream_raw(const link_side_t *side, frame_stream_t *stream, const char *reason)
{
    if (stream->length == 0U) {
        return;
    }
    if (stream->synchronized) {
        taskENTER_CRITICAL(&s_lock);
        ++s_state.framing_errors;
        taskEXIT_CRITICAL(&s_lock);
    }
#if KLIMA_BRIDGE_ACTIVE && !KLIMA_TAP_INJECT && !KLIMA_HYBRID_BRIDGE
    if (side->side == STREAM_PANEL) {
        forward_bytes(MAIN_UART, stream->bytes, stream->length);
        } else if (!KLIMA_MAIN_EDGE_FORWARD && !KLIMA_HYBRID_BRIDGE) {
            forward_bytes(PANEL_UART, stream->bytes, stream->length);
        }
#endif
    if (stream->synchronized) {
        publish_frame(side->name, stream->bytes, stream->length, reason);
    }
    stream->length = 0U;
    stream->expected = 0U;
}

static void consume_byte(const link_side_t *side, frame_stream_t *stream, uint8_t byte)
{
    bool retry = true;
    while (retry) {
        retry = false;
        if (stream->length == 0U) {
            if (byte == AC_PROTOCOL_HEADER_0) {
                stream->bytes[stream->length++] = byte;
#if KLIMA_HYBRID_BRIDGE && !KLIMA_SYNCED_TX_PROBE
                if (side->side == STREAM_PANEL) {
                    hybrid_forward_panel_byte(stream, 0U, byte);
                }
#endif
            } else {
#if KLIMA_HYBRID_BRIDGE && !KLIMA_SYNCED_TX_PROBE
                if (side->side == STREAM_PANEL) {
                    forward_bytes(MAIN_UART, &byte, 1U);
                }
#elif KLIMA_BRIDGE_ACTIVE && !KLIMA_TAP_INJECT
                if (side->side == STREAM_PANEL) {
                    forward_bytes(MAIN_UART, &byte, 1U);
                }
#endif
                if (stream->synchronized) {
                    taskENTER_CRITICAL(&s_lock);
                    ++s_state.framing_errors;
                    taskEXIT_CRITICAL(&s_lock);
                }
            }
            stream->last_byte_us = esp_timer_get_time();
            continue;
        }

        if (stream->length == 1U && byte != AC_PROTOCOL_HEADER_1) {
            flush_stream_raw(side, stream, "bad_header");
            retry = true;
            continue;
        }

        if (stream->length >= sizeof(stream->bytes)) {
            flush_stream_raw(side, stream, "overflow");
            retry = true;
            continue;
        }
#if KLIMA_HYBRID_BRIDGE && !KLIMA_SYNCED_TX_PROBE
        const size_t byte_index = stream->length;
#endif
        stream->bytes[stream->length++] = byte;
#if KLIMA_HYBRID_BRIDGE && !KLIMA_SYNCED_TX_PROBE
        if (side->side == STREAM_PANEL) {
            hybrid_forward_panel_byte(stream, byte_index, byte);
        }
#endif
        stream->last_byte_us = esp_timer_get_time();

        if (stream->length == 3U) {
            stream->expected = ac_protocol_expected_size(stream->bytes, stream->length);
            if (stream->expected == 0U) {
                flush_stream_raw(side, stream, "bad_length");
            }
        }
        if (stream->expected > 0U && stream->length == stream->expected) {
            const bool valid = ac_protocol_checksum_valid(stream->bytes, stream->length);
            if (stream->synchronized || valid) {
                process_complete_frame(side, stream->bytes, stream->length);
            }
            if (valid) {
                stream->synchronized = true;
            }
            stream->length = 0U;
            stream->expected = 0U;
        }
    }
}

static void stream_task(void *arg)
{
    const link_side_t *side = (const link_side_t *)arg;
    frame_stream_t stream = {0};
    uint8_t incoming[BRIDGE_READ_CHUNK_SIZE];

    telemetry_publishf(
        "LINK,event=started,direction=%s,uart=%d,rx_gpio=%d,tx_gpio=%d,profile=%s",
        side->name,
        side->uart,
        side->rx_gpio,
#if KLIMA_BRIDGE_ACTIVE
        side->tx_gpio,
#else
        -1,
#endif
        bridge_service_profile_name());

    while (true) {
        const int received = uart_read_bytes(
            side->uart,
            incoming,
            sizeof(incoming),
            pdMS_TO_TICKS(10));
        if (received > 0) {
            taskENTER_CRITICAL(&s_lock);
            if (side->side == STREAM_MAIN) {
                s_state.main_rx_bytes += (uint32_t)received;
            } else {
                s_state.panel_rx_bytes += (uint32_t)received;
            }
            taskEXIT_CRITICAL(&s_lock);
        }
        for (int i = 0; i < received; ++i) {
            if (side->side == STREAM_PANEL) {
                __atomic_store_n(
                    &s_panel_last_byte_us,
                    esp_timer_get_time(),
                    __ATOMIC_RELEASE);
            }
            consume_byte(side, &stream, incoming[i]);
        }
        if (received <= 0 && stream.length > 0U &&
            esp_timer_get_time() - stream.last_byte_us > BRIDGE_STREAM_TIMEOUT_US) {
            flush_stream_raw(side, &stream, "timeout");
        }
    }
}

#if KLIMA_BRIDGE_ACTIVE && KLIMA_HEADLESS_PROBE
static void virtual_panel_task(void *arg)
{
    (void)arg;
    uint8_t frame[AC_PANEL_FRAME_SIZE] = {
        0x54, 0x44, 0x0B, 0x11, 0x31, 0x00, 0x12, 0x00,
        0x00, 0x40, 0x00, 0x00, 0x00, 0x00,
    };
#if KLIMA_HEADLESS_MAIN22_PROBE
    const uint8_t main_off[AC_MAIN_FRAME_SIZE] = {
        0x54, 0x44, 0x12, 0x11, 0x31, 0x00, 0x12, 0x00,
        0x00, 0x00, 0x00, 0x2D, 0x2D, 0x00, 0x40, 0x0A,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x6A,
    };
    const uint8_t main_on[AC_MAIN_FRAME_SIZE] = {
        0x54, 0x44, 0x12, 0x11, 0x31, 0x80, 0x12, 0x00,
        0x02, 0x00, 0x00, 0x2C, 0x29, 0x00, 0x40, 0x0A,
        0x00, 0x00, 0x00, 0x00, 0x00, 0xED,
    };
#endif
    bool announced = false;

#if KLIMA_VIRTUAL_PANEL_EARLY_BOOT
    /* A physical display transmits as soon as it boots. Send the captured OFF
     * state before waiting for a main-board reply so a peer that latches a
     * missing-display fault on its first transaction can still synchronize. */
    frame[3] = 0x12U;
    ac_protocol_finalize(frame, sizeof(frame));
    forward_bytes(MAIN_UART, frame, KLIMA_VIRTUAL_PANEL_WIRE_SIZE);
    vTaskDelay(pdMS_TO_TICKS(20));
    frame[3] = 0x11U;
    ac_protocol_finalize(frame, sizeof(frame));
    forward_bytes(MAIN_UART, frame, KLIMA_VIRTUAL_PANEL_WIRE_SIZE);
    update_panel_state(frame, frame, false, true);
    telemetry_publishf(
        "VIRTUAL_PANEL,event=early_boot_frame,wire_len=%u",
        (unsigned)KLIMA_VIRTUAL_PANEL_WIRE_SIZE);
#endif

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(BRIDGE_VIRTUAL_PANEL_PERIOD_MS));
        const int64_t now = esp_timer_get_time();
        bool main_fresh;
        bool physical_panel_fresh;
        control_overlay_t overlay;

        taskENTER_CRITICAL(&s_lock);
        main_fresh = s_main_frame_time_us != 0 &&
            now - s_main_frame_time_us <= BRIDGE_LINK_FRESH_MS * 1000LL;
        physical_panel_fresh = s_physical_panel_frame_time_us != 0 &&
            now - s_physical_panel_frame_time_us <= BRIDGE_LINK_FRESH_MS * 1000LL;
        overlay = s_overlay;
        if (overlay.active && overlay.event_pending) {
            s_overlay.event_pending = false;
        }
        taskEXIT_CRITICAL(&s_lock);

        if ((!KLIMA_VIRTUAL_PANEL_EARLY_BOOT && !main_fresh) || physical_panel_fresh ||
            now < BRIDGE_VIRTUAL_PANEL_START_MS * 1000LL) {
            announced = false;
            continue;
        }
        if (!announced) {
            telemetry_publishf(
                "VIRTUAL_PANEL,event=started,reason=physical_panel_absent,period_ms=%u",
                BRIDGE_VIRTUAL_PANEL_PERIOD_MS);
            announced = true;
        }

        bool injected = false;
        if (overlay.active) {
            /* Match the authentic panel sequence: exactly one 0x12 event frame,
             * then stable 0x11 frames carrying the requested state. */
            injected = ac_protocol_apply_control(
                frame, &overlay.request, overlay.event_pending);
        } else {
            frame[3] = 0x11U;
            ac_protocol_finalize(frame, sizeof(frame));
        }
#if KLIMA_HEADLESS_MAIN22_PROBE
        uint8_t replay[AC_MAIN_FRAME_SIZE];
        const bool replay_on = overlay.active &&
            (overlay.request.mask & AC_CONTROL_POWER) != 0U &&
            overlay.request.power;
        memcpy(replay, replay_on ? main_on : main_off, sizeof(replay));
        replay[3] = overlay.active && overlay.event_pending ? 0x12U : 0x11U;
        ac_protocol_finalize(replay, sizeof(replay));
        forward_bytes(MAIN_UART, replay, sizeof(replay));
#else
        forward_bytes(MAIN_UART, frame, KLIMA_VIRTUAL_PANEL_WIRE_SIZE);
#endif
        update_panel_state(frame, frame, injected, true);
        if (injected && overlay.event_pending) {
            publish_frame(
                "panel_to_main", frame, sizeof(frame), "wifi_command");
        }
    }
}
#endif

#if KLIMA_BRIDGE_ACTIVE
static void control_takeover_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* CONFIG_FREERTOS_HZ is 100, therefore pdMS_TO_TICKS(1) is zero and
         * would turn this high-priority task into a scheduler-starving loop. */
        vTaskDelay(1);
        const int64_t now = esp_timer_get_time();
        bool pending = false;
        int64_t queued_time = 0;
        int64_t last_byte = __atomic_load_n(
            &s_panel_last_byte_us,
            __ATOMIC_ACQUIRE);
        taskENTER_CRITICAL(&s_lock);
        pending = s_overlay.active && s_overlay.takeover_pending;
        queued_time = s_overlay.queued_time_us;
        taskEXIT_CRITICAL(&s_lock);
        if (!pending) {
            continue;
        }

        /* The first command is only taken over after a complete UART idle
         * gap.  This is intentionally a pre-trigger: no byte of the next
         * physical panel frame can have reached ETM when the route is gated.
         */
        const int64_t reference = last_byte != 0 ? last_byte : queued_time;
        if (reference == 0 || now - reference < BRIDGE_STREAM_TIMEOUT_US) {
            continue;
        }
        const esp_err_t result =
            set_panel_to_main_hardware_mirror_enabled(false);
        taskENTER_CRITICAL(&s_lock);
        if (result == ESP_OK && s_overlay.active && s_overlay.takeover_pending) {
            s_overlay.takeover_pending = false;
            s_overlay.takeover_ready = true;
        } else if (result != ESP_OK && s_overlay.active) {
            s_overlay.active = false;
            s_overlay.event_pending = false;
            s_overlay.confirmation_pending = false;
            s_overlay.takeover_pending = false;
            s_overlay.takeover_ready = false;
            s_state.override_active = false;
            s_state.command_pending = false;
            s_state.command_status = BRIDGE_COMMAND_CANCELLED;
        }
        taskEXIT_CRITICAL(&s_lock);
        if (result != ESP_OK) {
            telemetry_publishf(
                "CONTROL,event=etm_takeover_failed,error=%s",
                esp_err_to_name(result));
        } else {
            telemetry_publishf(
                "CONTROL,event=etm_takeover_ready,idle_us=%" PRId64,
                now - reference);
        }
    }
}

static void control_watchdog_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        const int64_t now = esp_timer_get_time();
        uint32_t timed_out_sequence = 0U;
        bool restore_panel_mirror = false;
        taskENTER_CRITICAL(&s_lock);
        if (s_overlay.confirmation_pending && s_overlay.queued_time_us > 0 &&
            (now - s_overlay.queued_time_us) / 1000LL >=
                BRIDGE_COMMAND_CONFIRM_TIMEOUT_MS) {
            timed_out_sequence = s_overlay.sequence;
            s_overlay.active = false;
            s_overlay.event_pending = false;
            s_overlay.confirmation_pending = false;
            s_overlay.takeover_pending = false;
            s_overlay.takeover_ready = false;
            s_overlay.queued_time_us = 0;
            s_overlay.request = (ac_control_request_t){0};
            s_state.override_active = false;
            s_state.command_pending = false;
            s_state.command_status = BRIDGE_COMMAND_TIMED_OUT;
            ++s_state.command_timeouts;
            restore_panel_mirror = true;
#if KLIMA_SYNCED_TX_PROBE
            s_sync_tx_active = false;
#endif
        }
        taskEXIT_CRITICAL(&s_lock);
        if (restore_panel_mirror) {
            const esp_err_t result =
                set_panel_to_main_hardware_mirror_enabled(true);
            if (result != ESP_OK) {
                telemetry_publishf(
                    "CONTROL,event=etm_restore_failed,error=%s",
                    esp_err_to_name(result));
            }
        }
        if (timed_out_sequence != 0U) {
            telemetry_publishf(
                "CONTROL,event=timeout,sequence=%" PRIu32 ",timeout_ms=%lld",
                timed_out_sequence,
                BRIDGE_COMMAND_CONFIRM_TIMEOUT_MS);
        }
    }
}
#endif

static void health_task(void *arg)
{
    (void)arg;
    bool have_previous = false;
    bool previous_main = false;
    bool previous_panel = false;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        bridge_ac_state_t state;
        bridge_service_get_ac_state(&state);
        if (!have_previous || state.main_valid != previous_main ||
            state.panel_valid != previous_panel) {
            char event[64];
            snprintf(
                event,
                sizeof(event),
                "link_state main=%u panel=%u",
                state.main_valid ? 1U : 0U,
                state.panel_valid ? 1U : 0U);
            (void)capture_store_record_system(event);
            previous_main = state.main_valid;
            previous_panel = state.panel_valid;
            have_previous = true;
        }
        telemetry_publishf(
            "BRIDGE_HEALTH,profile=%s,main=%d,panel=%d,main_age_ms=%" PRId64
            ",panel_age_ms=%" PRId64 ",main_frames=%" PRIu32
            ",panel_frames=%" PRIu32 ",main_rx_bytes=%" PRIu32
            ",panel_rx_bytes=%" PRIu32 ",main_rx_level=%d,panel_rx_level=%d"
            ",checksum_errors=%" PRIu32
            ",framing_errors=%" PRIu32 ",panel_tx_pad_edges=%" PRIu32
            ",main_tx_pad_edges=%" PRIu32
            ",override=%d,pending=%d,sequence=%" PRIu32,
            bridge_service_profile_name(),
            state.main_valid,
            state.panel_valid,
            state.main_age_ms,
            state.panel_age_ms,
            state.main_frames,
            state.panel_frames,
            state.main_rx_bytes,
            state.panel_rx_bytes,
            state.main_rx_level,
            state.panel_rx_level,
            state.checksum_errors,
            state.framing_errors,
            state.panel_tx_pad_edges,
            state.main_tx_pad_edges,
            state.override_active,
            state.command_pending,
            state.command_sequence);
    }
}

void bridge_service_get_ac_state(bridge_ac_state_t *state)
{
    if (!state) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    *state = s_state;
    const int64_t main_time = s_main_frame_time_us;
    const int64_t panel_time = s_panel_frame_time_us;
    taskEXIT_CRITICAL(&s_lock);
    state->main_age_ms = main_time ? (now - main_time) / 1000LL : -1;
    state->panel_age_ms = panel_time ? (now - panel_time) / 1000LL : -1;
#if KLIMA_BRIDGE_ACTIVE
    state->main_rx_level = (int8_t)gpio_get_level(MAIN_RX_GPIO);
    state->panel_rx_level = (int8_t)gpio_get_level(PANEL_RX_GPIO);
    state->panel_candidate_gpio5_level = (int8_t)gpio_get_level(PANEL_TX_GPIO);
    state->alternate_panel_rx_level = (int8_t)gpio_get_level(MAIN_TX_GPIO);
    state->panel_tx_pad_level = (int8_t)gpio_get_level(PANEL_TX_GPIO);
    state->main_tx_pad_level = (int8_t)gpio_get_level(MAIN_TX_GPIO);
    state->panel_tx_pad_edges =
        __atomic_load_n(&s_panel_tx_pad_edges, __ATOMIC_RELAXED);
    state->main_tx_pad_edges =
        __atomic_load_n(&s_main_tx_pad_edges, __ATOMIC_RELAXED);
#if KLIMA_A4_B4_DIAGNOSTIC
    state->a4_monitor_level = (int8_t)gpio_get_level(A4_MONITOR_GPIO);
    state->b4_monitor_level = (int8_t)gpio_get_level(B4_MONITOR_GPIO);
    state->a4_monitor_edges =
        __atomic_load_n(&s_a4_monitor_edges, __ATOMIC_RELAXED);
    state->b4_monitor_edges =
        __atomic_load_n(&s_b4_monitor_edges, __ATOMIC_RELAXED);
    state->b4_monitor_bytes =
        __atomic_load_n(&s_b4_monitor_bytes, __ATOMIC_RELAXED);
    state->b4_monitor_frames =
        __atomic_load_n(&s_b4_monitor_frames, __ATOMIC_RELAXED);
    state->b4_monitor_checksum_errors =
        __atomic_load_n(&s_b4_monitor_checksum_errors, __ATOMIC_RELAXED);
    state->b4_low_last_us =
        __atomic_load_n(&s_b4_low_last_us, __ATOMIC_RELAXED);
    state->b4_low_min_us =
        __atomic_load_n(&s_b4_low_min_us, __ATOMIC_RELAXED);
    state->b4_low_max_us =
        __atomic_load_n(&s_b4_low_max_us, __ATOMIC_RELAXED);
    state->b4_capture_lateness_us =
        __atomic_load_n(&s_b4_capture_lateness_us, __ATOMIC_RELAXED);
    taskENTER_CRITICAL(&s_lock);
    memcpy(state->b4_monitor_raw, s_b4_monitor_raw, AC_PANEL_FRAME_SIZE);
    taskEXIT_CRITICAL(&s_lock);
#else
    state->a4_monitor_level = -1;
    state->b4_monitor_level = -1;
#endif
#if KLIMA_CONVERTER_LOOPBACK_PROBE
    state->alternate_panel_rx_level =
        (int8_t)gpio_get_level(CONVERTER_LOOPBACK_GPIO);
    state->alternate_panel_edges =
        __atomic_load_n(&s_converter_loopback_edges, __ATOMIC_RELAXED);
#endif
#if KLIMA_MAIN_EDGE_FORWARD && !KLIMA_MAIN_HARDWARE_FORWARD
    state->mirror_main_edges =
        __atomic_load_n(&s_probe_main_edges, __ATOMIC_RELAXED);
#endif
#elif KLIMA_PANEL_BENCH_EMULATOR
    state->main_rx_level = (int8_t)gpio_get_level(MAIN_RX_GPIO);
    state->panel_rx_level = (int8_t)gpio_get_level(PANEL_RX_GPIO);
    state->panel_tx_pad_level = (int8_t)gpio_get_level(PANEL_TX_GPIO);
    state->main_tx_pad_level = (int8_t)gpio_get_level(MAIN_TX_GPIO);
#elif KLIMA_ALL_INPUTS_PROBE
    state->main_rx_level = (int8_t)gpio_get_level(GPIO_NUM_4);
    state->panel_rx_level = (int8_t)gpio_get_level(GPIO_NUM_5);
    state->panel_candidate_gpio5_level = (int8_t)gpio_get_level(GPIO_NUM_6);
    state->alternate_panel_rx_level = (int8_t)gpio_get_level(GPIO_NUM_7);
    state->mirror_main_edges =
        __atomic_load_n(&s_all_input_edges[0], __ATOMIC_RELAXED);
    state->panel_candidate_gpio5_edges =
        __atomic_load_n(&s_all_input_edges[1], __ATOMIC_RELAXED);
    state->alternate_panel_edges =
        __atomic_load_n(&s_all_input_edges[2], __ATOMIC_RELAXED);
    state->injected_frames =
        __atomic_load_n(&s_all_input_edges[3], __ATOMIC_RELAXED);
#elif KLIMA_BRIDGE_MIRROR_PROBE
    state->main_rx_level = (int8_t)gpio_get_level(PROBE_MAIN_RX_GPIO);
    state->panel_rx_level = (int8_t)gpio_get_level(PROBE_PANEL_RX_GPIO);
    state->alternate_panel_rx_level =
        (int8_t)gpio_get_level(PROBE_ALTERNATE_PANEL_RX_GPIO);
    state->panel_candidate_gpio5_level =
        (int8_t)gpio_get_level(PROBE_PANEL_CANDIDATE_GPIO5);
    state->mirror_main_edges = __atomic_load_n(&s_probe_main_edges, __ATOMIC_RELAXED);
    state->alternate_panel_edges =
        __atomic_load_n(&s_probe_alternate_panel_edges, __ATOMIC_RELAXED);
    state->panel_candidate_gpio5_edges =
        __atomic_load_n(&s_probe_panel_gpio5_edges, __ATOMIC_RELAXED);
#elif KLIMA_WAKE7_PROBE
    state->main_rx_level = (int8_t)gpio_get_level(GPIO_NUM_4);
    state->panel_rx_level = (int8_t)gpio_get_level(GPIO_NUM_5);
    state->panel_candidate_gpio5_level = (int8_t)gpio_get_level(GPIO_NUM_6);
    state->alternate_panel_rx_level = (int8_t)gpio_get_level(GPIO_NUM_7);
    state->mirror_main_edges =
        __atomic_load_n(&s_probe_main_edges, __ATOMIC_RELAXED);
    state->panel_candidate_gpio5_edges =
        __atomic_load_n(&s_probe_panel_gpio5_edges, __ATOMIC_RELAXED);
#else
    state->main_rx_level = (int8_t)gpio_get_level(PASSIVE_MAIN_RX_GPIO);
    state->panel_rx_level = (int8_t)gpio_get_level(PASSIVE_PANEL_RX_GPIO);
    state->panel_candidate_gpio5_level = (int8_t)gpio_get_level(GPIO_NUM_6);
    state->alternate_panel_rx_level = (int8_t)gpio_get_level(GPIO_NUM_7);
    state->panel_candidate_gpio5_edges =
        __atomic_load_n(&s_passive_aux_edges[0], __ATOMIC_RELAXED);
    state->alternate_panel_edges =
        __atomic_load_n(&s_passive_aux_edges[1], __ATOMIC_RELAXED);
#endif
    state->main_valid = state->main_valid && state->main_age_ms <= BRIDGE_LINK_FRESH_MS;
    state->panel_valid = state->panel_valid && state->panel_age_ms <= BRIDGE_LINK_FRESH_MS;
}

esp_err_t bridge_service_queue_control(
    const ac_control_request_t *request,
    uint32_t *sequence)
{
    if (!request || request->mask == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
#if !KLIMA_BRIDGE_ACTIVE
    (void)sequence;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!bridge_service_mitm_active()) {
        (void)sequence;
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((request->mask & AC_CONTROL_SETPOINT) != 0U &&
        !ac_protocol_valid_setpoint(request->setpoint_c)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((request->mask & AC_CONTROL_MODE) != 0U &&
        request->mode != AC_MODE_COOL && request->mode != AC_MODE_FAN &&
        request->mode != AC_MODE_DRY) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((request->mask & AC_CONTROL_FAN) != 0U &&
        request->fan != AC_FAN_LOW && request->fan != AC_FAN_HIGH) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The active bit was captured, but the accompanying timer duration field
     * is still unknown. A bit-only command is rejected by the real main board,
     * so production control must fail closed instead of exposing a switch that
     * always times out. */
    if ((request->mask & AC_CONTROL_TIMER) != 0U) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    const bool links_fresh = s_state.main_valid && s_state.panel_valid &&
        s_main_frame_time_us > 0 && s_panel_frame_time_us > 0 &&
        (now - s_main_frame_time_us) / 1000LL <= BRIDGE_LINK_FRESH_MS &&
        (now - s_panel_frame_time_us) / 1000LL <= BRIDGE_LINK_FRESH_MS;
    if (!links_fresh) {
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if ((request->mask & AC_CONTROL_SETPOINT) != 0U) {
        const ac_mode_t requested_mode = (request->mask & AC_CONTROL_MODE) != 0U
            ? request->mode
            : s_state.mode;
        if (requested_mode != AC_MODE_COOL) {
            taskEXIT_CRITICAL(&s_lock);
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (!s_overlay.active) {
        s_overlay.request = (ac_control_request_t){0};
    }
    if ((request->mask & AC_CONTROL_POWER) != 0U) {
        s_overlay.request.power = request->power;
    }
    if ((request->mask & AC_CONTROL_SETPOINT) != 0U) {
        s_overlay.request.setpoint_c = request->setpoint_c;
    }
    if ((request->mask & AC_CONTROL_MODE) != 0U) {
        s_overlay.request.mode = request->mode;
        s_overlay.request.mask &= ~AC_CONTROL_RAW_MODE_FAN;
    }
    if ((request->mask & AC_CONTROL_FAN) != 0U) {
        s_overlay.request.fan = request->fan;
        s_overlay.request.mask &= ~AC_CONTROL_RAW_MODE_FAN;
    }
    if ((request->mask & AC_CONTROL_RAW_MODE_FAN) != 0U) {
        s_overlay.request.raw_mode_fan_code = request->raw_mode_fan_code;
        s_overlay.request.mask &= ~(AC_CONTROL_MODE | AC_CONTROL_FAN);
    }
    if ((request->mask & AC_CONTROL_QUIET) != 0U) {
        s_overlay.request.quiet = request->quiet;
    }
    if ((request->mask & AC_CONTROL_UNITS_FAHRENHEIT) != 0U) {
        s_overlay.request.units_fahrenheit = request->units_fahrenheit;
    }
    if ((request->mask & AC_CONTROL_TIMER) != 0U) {
        s_overlay.request.timer = request->timer;
    }
    s_overlay.request.mask |= request->mask;
    s_overlay.active = true;
    s_overlay.event_pending = true;
    s_overlay.confirmation_pending = true;
    s_overlay.takeover_pending =
        KLIMA_PANEL_HARDWARE_FORWARD != 0 && KLIMA_PANEL_LOOP_CONTROL == 0;
    s_overlay.takeover_ready =
        KLIMA_PANEL_HARDWARE_FORWARD == 0 && KLIMA_PANEL_LOOP_CONTROL == 0;
    s_overlay.queued_time_us = now;
    ++s_overlay.sequence;
    s_state.override_active = true;
    s_state.command_pending = true;
    s_state.command_status = BRIDGE_COMMAND_PENDING;
    s_state.command_sequence = s_overlay.sequence;
    const uint32_t assigned = s_overlay.sequence;
#if KLIMA_SYNCED_TX_PROBE
    if (!sync_prepare_control_frames_locked()) {
        s_overlay.active = false;
        s_overlay.event_pending = false;
        s_overlay.confirmation_pending = false;
        s_overlay.takeover_pending = false;
        s_overlay.takeover_ready = false;
        s_overlay.queued_time_us = 0;
        s_state.override_active = false;
        s_state.command_pending = false;
        s_state.command_status = BRIDGE_COMMAND_CANCELLED;
        s_sync_tx_active = false;
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
#endif
    taskEXIT_CRITICAL(&s_lock);

    if (sequence) {
        *sequence = assigned;
    }
    telemetry_publishf(
        "CONTROL,event=queued,sequence=%" PRIu32 ",mask=0x%02" PRIX32,
        assigned,
        request->mask);
    return ESP_OK;
#endif
}

bool bridge_service_mitm_active(void)
{
    return KLIMA_BRIDGE_ACTIVE != 0 && KLIMA_CONTROL_VERIFIED != 0 &&
        s_hardware_ready;
}

void bridge_service_set_hardware_ready(bool ready)
{
    taskENTER_CRITICAL(&s_lock);
    s_hardware_ready = ready;
    s_state.mitm_active = bridge_service_mitm_active();
    s_boot_force_off_until_us = KLIMA_BOOT_FORCE_OFF_DIAG
        ? esp_timer_get_time() + 8000000LL
        : 0;
    if (!ready && s_overlay.active) {
        s_overlay.active = false;
        s_overlay.event_pending = false;
        s_overlay.confirmation_pending = false;
        s_overlay.takeover_pending = false;
        s_overlay.takeover_ready = false;
        s_overlay.queued_time_us = 0;
        s_state.override_active = false;
        s_state.command_pending = false;
        s_state.command_status = BRIDGE_COMMAND_CANCELLED;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!ready) {
        (void)set_panel_to_main_hardware_mirror_enabled(true);
    }
}

const char *bridge_service_command_status_name(bridge_command_status_t status)
{
    switch (status) {
    case BRIDGE_COMMAND_PENDING:
        return "pending";
    case BRIDGE_COMMAND_CONFIRMED:
        return "confirmed";
    case BRIDGE_COMMAND_TIMED_OUT:
        return "timed_out";
    case BRIDGE_COMMAND_CANCELLED:
        return "cancelled";
    default:
        return "none";
    }
}

const char *bridge_service_profile_name(void)
{
#if KLIMA_GPIO0_UART_DIAG
    return "gpio0-uart-diag";
#elif KLIMA_CANONICAL_MITM_NTS
    return "mitm-nts";
#elif KLIMA_CANONICAL_BRIDGE
    return "bridge";
#elif KLIMA_CANONICAL_PANEL_DIAG
    return "panel-diag";
#elif KLIMA_CANONICAL_PANEL_BENCH
    return "panel-bench";
#elif KLIMA_CANONICAL_SNIFFER
    return "sniffer";
#elif KLIMA_SYNCED_TX_PROBE && KLIMA_TX_OPEN_DRAIN
    return "hybrid-sync-od-hil";
#elif KLIMA_SYNCED_TX_PROBE
    return "hybrid-sync-hil";
#elif KLIMA_PANEL_HARDWARE_FORWARD
    return "hybrid-hardware-hil";
#elif KLIMA_HYBRID_BRIDGE
    return "hybrid-bridge";
#elif KLIMA_TAP_INJECT
    return "tap-inject";
#elif KLIMA_BRIDGE_ACTIVE && KLIMA_HEADLESS_PROBE
    return "headless-probe";
#elif KLIMA_BRIDGE_ACTIVE
    return "mitm";
#elif KLIMA_ALL_INPUTS_PROBE
    return "all-inputs-probe";
#elif KLIMA_HARDWARE_BYPASS_PROBE
    return "hardware-bypass-probe";
#elif KLIMA_WAKE7_PROBE
    return KLIMA_WAKE_DEST_GPIO_NUM == 6 ? "wake6-probe" : "wake7-probe";
#elif KLIMA_BRIDGE_MIRROR_PROBE
    return "mirror-probe";
#else
    return "safe-passive";
#endif
}

esp_err_t bridge_service_start(void)
{
    memset(&s_state, 0, sizeof(s_state));
#if KLIMA_NTS0104_PREFLIGHT
    s_hardware_ready = false;
#else
    s_hardware_ready = true;
#endif
    s_state.mitm_active = bridge_service_mitm_active();
    s_physical_panel_frame_time_us = 0;
    s_panel_last_byte_us = 0;
#if KLIMA_TAP_INJECT
    s_tap_last_injected_time_us = 0;
    s_tap_echo_pending = false;
#endif
#if KLIMA_GPIO0_UART_DIAG
    /* OE is held low by the canonical preflight profile, so this test reaches
     * only the low-side GPIO0/A4 wire. GPIO5 is input-only and observes A4.
     * The disabled translator cannot drive B4 or the appliance. */
    esp_err_t diag_isr_result = gpio_install_isr_service(0);
    if (diag_isr_result != ESP_OK && diag_isr_result != ESP_ERR_INVALID_STATE) {
        return diag_isr_result;
    }
    const gpio_config_t a4_input = {
        .pin_bit_mask = 1ULL << A4_MONITOR_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&a4_input), "bridge", "configure A4 diagnostic input");
    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(
            A4_MONITOR_GPIO, gpio0_diag_edge_isr,
            (void *)&s_gpio0_diag_a4_edges),
        "bridge",
        "monitor A4 diagnostic edges");
    ESP_RETURN_ON_ERROR(
        configure_uart(MAIN_UART, MAIN_TX_GPIO, MAIN_RX_GPIO, true),
        "bridge",
        "configure isolated GPIO0 UART diagnostic");
    gpio_ll_od_disable(&GPIO, MAIN_TX_GPIO);
    ESP_RETURN_ON_ERROR(
        gpio_set_drive_capability(MAIN_TX_GPIO, GPIO_DRIVE_CAP_3),
        "bridge",
        "set GPIO0 diagnostic drive strength");
    ESP_RETURN_ON_ERROR(
        monitor_active_tx_pad(MAIN_TX_GPIO, &s_gpio0_diag_tx_edges),
        "bridge",
        "monitor GPIO0 diagnostic output");
    BaseType_t diag_created = xTaskCreate(
        gpio0_uart_diag_task, "gpio0_uart_diag", 3072, NULL, 12, NULL);
    if (diag_created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    telemetry_publishf(
        "GPIO0_UART_DIAG,event=started,uart=%d,tx_gpio=%d,a4_monitor_gpio=%d,oe=low",
        MAIN_UART, MAIN_TX_GPIO, A4_MONITOR_GPIO);
    return ESP_OK;
#endif
#if KLIMA_PANEL_HARDWARE_FORWARD
    ESP_RETURN_ON_ERROR(
        configure_uart(MAIN_UART, GPIO_NUM_NC, MAIN_RX_GPIO, false),
        "bridge",
        "hardware-forward main RX UART");
    ESP_RETURN_ON_ERROR(
        start_hardware_mirror(
            PANEL_RX_GPIO, MAIN_TX_GPIO, &s_hardware_mirrors[0]),
        "bridge",
        "start hardware panel-to-main forward");
    /* This mirror is the panel-source (GPIO7) to main-destination
     * (MAIN_TX_GPIO) route. Keep it enabled until a command is queued. */
    s_panel_to_main_hardware_mirror_enabled = true;
#elif KLIMA_SYNCED_TX_PROBE
    s_sync_tx_active = false;
    s_sync_tx_ready = false;
    s_sync_tx_event_pending = false;
    s_sync_tx_sequence = 0U;
    s_sync_tx_attempt = 0U;
#endif

#if KLIMA_BRIDGE_ACTIVE
    esp_err_t active_isr_result = gpio_install_isr_service(0);
    if (active_isr_result != ESP_OK && active_isr_result != ESP_ERR_INVALID_STATE) {
        return active_isr_result;
    }
#if KLIMA_TAP_INJECT || (KLIMA_HYBRID_BRIDGE && KLIMA_MAIN_EDGE_FORWARD)
    ESP_RETURN_ON_ERROR(gpio_reset_pin(PANEL_TX_GPIO), "bridge", "release panel TX pad");
    ESP_RETURN_ON_ERROR(
        gpio_set_direction(PANEL_TX_GPIO, GPIO_MODE_INPUT),
        "bridge",
        "keep tap GPIO6 input-only");
    ESP_RETURN_ON_ERROR(
        gpio_set_pull_mode(PANEL_TX_GPIO, GPIO_PULLUP_ONLY),
        "bridge",
        "panel TX pull-up");
    ESP_RETURN_ON_ERROR(
        configure_uart(PANEL_UART, GPIO_NUM_NC, PANEL_RX_GPIO, false),
        "bridge",
        "edge-forward main-to-panel RX UART");
#else
    ESP_RETURN_ON_ERROR(
        configure_uart(
            PANEL_UART,
            KLIMA_MAIN_EDGE_FORWARD ? GPIO_NUM_NC : PANEL_TX_GPIO,
            PANEL_RX_GPIO,
            !KLIMA_MAIN_EDGE_FORWARD),
        "bridge",
        "panel UART");
#endif
#if KLIMA_HYBRID_BRIDGE
    /* Detach the previous pad function before routing UART1 TX to the
     * main-board input. Canonical profiles use GPIO0 to avoid the GPIO5
     * strapping/JTAG pad. When PANEL_HARDWARE_FORWARD is active MAIN_TX_GPIO is
     * already owned by the
     * ETM panel->main destination; resetting the pad here would tear down the
     * transparent route before its first frame. */
#if !KLIMA_PANEL_HARDWARE_FORWARD
    ESP_RETURN_ON_ERROR(gpio_reset_pin(MAIN_TX_GPIO), "bridge", "reset main TX pad");
#endif
#endif
#if KLIMA_SYNCED_TX_PROBE
    ESP_RETURN_ON_ERROR(
        configure_uart(MAIN_UART, GPIO_NUM_NC, MAIN_RX_GPIO, false),
        "bridge",
        "synchronized main RX UART");
    ESP_RETURN_ON_ERROR(
        start_synced_tx_output(),
        "bridge",
        "start synchronized main TX");
    BaseType_t sync_created = xTaskCreate(
        sync_tx_task,
        "sync_tx",
        4096,
        NULL,
        20,
        &s_sync_tx_task_handle);
    if (sync_created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t sync_timer_config = {
        .callback = sync_tx_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "panel_reply",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(
        esp_timer_create(&sync_timer_config, &s_sync_tx_timer),
        "bridge",
        "create synchronized panel reply timer");
    telemetry_publishf(
        "SYNC_TX,event=scheduled_from_main,delay_us=%u",
        (unsigned)SYNC_TX_AFTER_MAIN_US);
#else
    ESP_RETURN_ON_ERROR(
        configure_uart(
            MAIN_UART,
#if KLIMA_PANEL_HARDWARE_FORWARD
            GPIO_NUM_NC,
#else
            MAIN_TX_GPIO,
#endif
            MAIN_RX_GPIO,
            true),
        "bridge",
        "main UART");
#if KLIMA_HYBRID_BRIDGE
#if KLIMA_MAIN_TX_OPEN_DRAIN
    gpio_ll_od_enable(&GPIO, MAIN_TX_GPIO);
    ESP_RETURN_ON_ERROR(
        gpio_set_pull_mode(MAIN_TX_GPIO, GPIO_PULLUP_ONLY),
        "bridge",
        "main TX open-drain pull-up");
#else
    gpio_ll_od_disable(&GPIO, MAIN_TX_GPIO);
#endif
    ESP_RETURN_ON_ERROR(
        gpio_set_drive_capability(MAIN_TX_GPIO, GPIO_DRIVE_CAP_3),
        "bridge",
        "set main TX drive strength");
#endif
#endif
#if KLIMA_TX_OPEN_DRAIN
#if !KLIMA_TAP_INJECT
    gpio_ll_od_enable(&GPIO, PANEL_TX_GPIO);
    ESP_RETURN_ON_ERROR(
        gpio_set_pull_mode(PANEL_TX_GPIO, GPIO_PULLUP_ONLY),
        "bridge",
        "panel TX pull-up");
#endif
    gpio_ll_od_enable(&GPIO, MAIN_TX_GPIO);
    ESP_RETURN_ON_ERROR(
        gpio_set_pull_mode(MAIN_TX_GPIO, GPIO_PULLUP_ONLY),
        "bridge",
        "main TX pull-up");
    telemetry_publishf(
        "BRIDGE,event=tx_electrical_mode,mode=open_drain,panel_tx=%d,main_tx=%d",
        KLIMA_TAP_INJECT ? -1 : PANEL_TX_GPIO,
        MAIN_TX_GPIO);
#endif
#if KLIMA_MAIN_EDGE_FORWARD
#if KLIMA_MAIN_HARDWARE_FORWARD
    ESP_RETURN_ON_ERROR(
        start_hardware_mirror(
            MAIN_RX_GPIO, PANEL_TX_GPIO,
#if KLIMA_PANEL_HARDWARE_FORWARD
            &s_hardware_mirrors[1]),
#else
            &s_hardware_mirrors[0]),
#endif
        "bridge",
        "start hardware main-to-panel forward");
#else
    s_main_to_panel_mirror.source = MAIN_RX_GPIO;
    s_main_to_panel_mirror.destination = PANEL_TX_GPIO;
    s_main_to_panel_mirror.counter = &s_probe_main_edges;
    ESP_RETURN_ON_ERROR(
        gpio_reset_pin(PANEL_TX_GPIO),
        "bridge",
        "reset main edge-forward pad");
    gpio_set_level(PANEL_TX_GPIO, gpio_get_level(MAIN_RX_GPIO));
    ESP_RETURN_ON_ERROR(
        gpio_set_direction(PANEL_TX_GPIO, GPIO_MODE_INPUT_OUTPUT),
        "bridge",
        "main edge-forward output");
    esp_err_t isr_result = gpio_install_isr_service(0);
    if (isr_result != ESP_OK && isr_result != ESP_ERR_INVALID_STATE) {
        return isr_result;
    }
    ESP_RETURN_ON_ERROR(
        gpio_set_intr_type(MAIN_RX_GPIO, GPIO_INTR_ANYEDGE),
        "bridge",
        "main edge-forward type");
    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(
            MAIN_RX_GPIO, mirror_edge_isr, &s_main_to_panel_mirror),
        "bridge",
        "main edge-forward ISR");
#endif
#endif
#if !KLIMA_TAP_INJECT && !KLIMA_HYBRID_BRIDGE
    ESP_RETURN_ON_ERROR(
        monitor_active_tx_pad(PANEL_TX_GPIO, &s_panel_tx_pad_edges),
        "bridge",
        "monitor panel TX pad");
#endif
    ESP_RETURN_ON_ERROR(
        monitor_active_tx_pad(MAIN_TX_GPIO, &s_main_tx_pad_edges),
        "bridge",
        "monitor main TX pad");
#if KLIMA_A4_B4_DIAGNOSTIC
    const gpio_config_t a4_b4_monitor = {
        .pin_bit_mask = (1ULL << A4_MONITOR_GPIO) | (1ULL << B4_MONITOR_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(
        gpio_config(&a4_b4_monitor),
        "bridge",
        "configure A4/B4 monitor inputs");
    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(
            A4_MONITOR_GPIO,
            count_active_tx_edge_isr,
            (void *)&s_a4_monitor_edges),
        "bridge",
        "monitor converter A4");
    ESP_RETURN_ON_ERROR(
        gpio_set_intr_type(B4_MONITOR_GPIO, GPIO_INTR_ANYEDGE),
        "bridge",
        "configure B4 timing edges");
    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(
            B4_MONITOR_GPIO,
            b4_monitor_edge_isr,
            NULL),
        "bridge",
        "monitor converter B4");
    BaseType_t b4_created = xTaskCreate(
        b4_monitor_task,
        "b4_monitor",
        3072,
        NULL,
        20,
        &s_b4_monitor_task_handle);
    if (b4_created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    telemetry_publishf(
        "B4_DIAG,event=started,a4_gpio=%d,b4_gpio=%d,decoder=software_uart",
        A4_MONITOR_GPIO,
        B4_MONITOR_GPIO);
#endif
#if KLIMA_CONVERTER_LOOPBACK_PROBE
    const gpio_config_t converter_loopback = {
        .pin_bit_mask = 1ULL << CONVERTER_LOOPBACK_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(
        gpio_config(&converter_loopback),
        "bridge",
        "configure converter loopback input");
    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(
            CONVERTER_LOOPBACK_GPIO,
            count_active_tx_edge_isr,
            (void *)&s_converter_loopback_edges),
        "bridge",
        "monitor converter HV loopback");
    telemetry_publishf(
        "LOOPBACK,event=started,gpio=%d", CONVERTER_LOOPBACK_GPIO);
#endif
#elif KLIMA_PANEL_BENCH_EMULATOR
    ESP_RETURN_ON_ERROR(
        configure_uart(PANEL_UART, PANEL_TX_GPIO, PANEL_RX_GPIO, true),
        "bridge",
        "detached panel bench UART");
    ESP_RETURN_ON_ERROR(
        configure_uart(MAIN_UART, GPIO_NUM_NC, MAIN_RX_GPIO, false),
        "bridge",
        "detached panel unused main RX UART");
#elif KLIMA_ALL_INPUTS_PROBE
    const gpio_config_t all_inputs = {
        .pin_bit_mask = (1ULL << GPIO_NUM_4) | (1ULL << GPIO_NUM_5) |
            (1ULL << GPIO_NUM_6) | (1ULL << GPIO_NUM_7),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&all_inputs), "bridge", "configure all-input probe");
    esp_err_t input_isr_result = gpio_install_isr_service(0);
    if (input_isr_result != ESP_OK && input_isr_result != ESP_ERR_INVALID_STATE) {
        return input_isr_result;
    }
    for (size_t i = 0; i < 4U; ++i) {
        ESP_RETURN_ON_ERROR(
            gpio_isr_handler_add(
                k_all_input_gpios[i], count_all_input_edge_isr,
                (void *)&s_all_input_edges[i]),
            "bridge",
            "install all-input probe ISR");
    }
    ESP_RETURN_ON_ERROR(
        configure_uart(PANEL_UART, GPIO_NUM_NC, ALL_INPUTS_PANEL_RX_GPIO, false),
        "bridge",
        "all-input panel UART");
    ESP_RETURN_ON_ERROR(
        configure_uart(MAIN_UART, GPIO_NUM_NC, ALL_INPUTS_MAIN_RX_GPIO, false),
        "bridge",
        "all-input main UART");
#elif KLIMA_WAKE7_PROBE
    const gpio_config_t wake7_release = {
        .pin_bit_mask = (1ULL << GPIO_NUM_4) | (1ULL << GPIO_NUM_5) |
            (1ULL << GPIO_NUM_6) | (1ULL << GPIO_NUM_7),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&wake7_release), "bridge", "release wake7 pins");
    ESP_RETURN_ON_ERROR(
        configure_uart(PANEL_UART, GPIO_NUM_NC, GPIO_NUM_5, false),
        "bridge", "wake7 panel UART");
    ESP_RETURN_ON_ERROR(
        configure_uart(MAIN_UART, GPIO_NUM_NC, GPIO_NUM_4, false),
        "bridge", "wake7 main UART");
    ESP_RETURN_ON_ERROR(gpio_reset_pin(WAKE_DEST_GPIO), "bridge", "reset wake pad");
    ESP_RETURN_ON_ERROR(
        gpio_set_direction(WAKE_DEST_GPIO, GPIO_MODE_INPUT_OUTPUT),
        "bridge",
        "configure wake output");
    gpio_set_level(WAKE_DEST_GPIO, gpio_get_level(GPIO_NUM_4));
    s_main_to_panel_mirror.source = GPIO_NUM_4;
    s_main_to_panel_mirror.destination = WAKE_DEST_GPIO;
    s_main_to_panel_mirror.counter = &s_probe_main_edges;
    esp_err_t wake7_isr_result = gpio_install_isr_service(0);
    if (wake7_isr_result != ESP_OK && wake7_isr_result != ESP_ERR_INVALID_STATE) {
        return wake7_isr_result;
    }
    ESP_RETURN_ON_ERROR(
        gpio_set_intr_type(GPIO_NUM_4, GPIO_INTR_ANYEDGE),
        "bridge",
        "configure wake7 source edges");
    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(GPIO_NUM_4, mirror_edge_isr, &s_main_to_panel_mirror),
        "bridge",
        "install wake7 forwarder");
    ESP_RETURN_ON_ERROR(
        gpio_set_intr_type(WAKE_ALT_GPIO, GPIO_INTR_ANYEDGE),
        "bridge",
        "configure wake7 alternate source edges");
    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(
            WAKE_ALT_GPIO, count_edge_isr, (void *)&s_probe_panel_gpio5_edges),
        "bridge",
        "install wake7 alternate source monitor");
#elif KLIMA_HARDWARE_BYPASS_PROBE
    /* Transparent final path for KAmodNTS0104PW.  ETM connects each input
     * edge to its opposite-side output without CPU ISR latency. UARTs observe
     * the two source endpoints; command injection temporarily disables only
     * the panel->main destination and restores it after confirmation/timeout. */
    const gpio_config_t release = {
        .pin_bit_mask = (1ULL << GPIO_NUM_4) | (1ULL << GPIO_NUM_5) |
            (1ULL << GPIO_NUM_6) | (1ULL << GPIO_NUM_7),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&release), "bridge", "release bypass GPIOs");
    ESP_RETURN_ON_ERROR(
        configure_uart(PANEL_UART, GPIO_NUM_NC, GPIO_NUM_7, false),
        "bridge",
        "hardware bypass panel UART");
    ESP_RETURN_ON_ERROR(
        configure_uart(MAIN_UART, GPIO_NUM_NC, GPIO_NUM_4, false),
        "bridge",
        "hardware bypass main UART");
    ESP_RETURN_ON_ERROR(
        start_hardware_mirror(GPIO_NUM_4, GPIO_NUM_6, &s_hardware_mirrors[0]),
        "bridge",
        "start main-to-panel ETM bypass");
    ESP_RETURN_ON_ERROR(
        start_hardware_mirror(GPIO_NUM_7, GPIO_NUM_5, &s_hardware_mirrors[1]),
        "bridge",
        "start panel-to-main ETM bypass");
#elif KLIMA_BRIDGE_MIRROR_PROBE
    /* Reproduce both former bypasses edge-for-edge while both UARTs remain
     * RX-only on the two candidate transmitter endpoints. */
    const gpio_config_t release = {
        .pin_bit_mask = (1ULL << GPIO_NUM_4) | (1ULL << GPIO_NUM_5) |
            (1ULL << GPIO_NUM_6) | (1ULL << GPIO_NUM_7),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&release), "bridge", "release probe GPIOs");
    ESP_RETURN_ON_ERROR(
        configure_uart(PANEL_UART, GPIO_NUM_NC, PROBE_PANEL_RX_GPIO, false),
        "bridge",
        "probe panel UART");
    ESP_RETURN_ON_ERROR(
        configure_uart(MAIN_UART, GPIO_NUM_NC, PROBE_MAIN_RX_GPIO, false),
        "bridge",
        "probe main UART");
    ESP_RETURN_ON_ERROR(
        gpio_reset_pin(PROBE_MAIN_TO_PANEL_GPIO),
        "bridge",
        "reset probe mirror pad");
    gpio_set_level(PROBE_MAIN_TO_PANEL_GPIO, gpio_get_level(PROBE_MAIN_RX_GPIO));
    ESP_RETURN_ON_ERROR(
        gpio_set_direction(PROBE_MAIN_TO_PANEL_GPIO, GPIO_MODE_INPUT_OUTPUT),
        "bridge",
        "probe mirror output");
    esp_err_t isr_result = gpio_install_isr_service(0);
    if (isr_result != ESP_OK && isr_result != ESP_ERR_INVALID_STATE) {
        return isr_result;
    }
    ESP_RETURN_ON_ERROR(
        gpio_set_intr_type(PROBE_MAIN_RX_GPIO, GPIO_INTR_ANYEDGE),
        "bridge",
        "probe main edge type");
    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(
            PROBE_MAIN_RX_GPIO, mirror_edge_isr, &s_main_to_panel_mirror),
        "bridge",
        "probe main mirror ISR");
    ESP_RETURN_ON_ERROR(
        gpio_set_intr_type(PROBE_PANEL_CANDIDATE_GPIO5, GPIO_INTR_ANYEDGE),
        "bridge",
        "probe candidate one edge type");
    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(
            PROBE_PANEL_CANDIDATE_GPIO5,
            count_edge_isr,
            (void *)&s_probe_panel_gpio5_edges),
        "bridge",
        "probe candidate one ISR");
    ESP_RETURN_ON_ERROR(
        gpio_set_intr_type(PROBE_ALTERNATE_PANEL_RX_GPIO, GPIO_INTR_ANYEDGE),
        "bridge",
        "probe candidate two edge type");
    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(
            PROBE_ALTERNATE_PANEL_RX_GPIO,
            count_edge_isr,
            (void *)&s_probe_alternate_panel_edges),
        "bridge",
        "probe candidate two ISR");
#else
    /* Release all potential TX pins before installing either RX-only UART. */
    const gpio_config_t release = {
        .pin_bit_mask = (1ULL << GPIO_NUM_4) | (1ULL << GPIO_NUM_5) |
            (1ULL << GPIO_NUM_6) | (1ULL << GPIO_NUM_7),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&release), "bridge", "release all link GPIOs");
    ESP_RETURN_ON_ERROR(
        configure_uart(PANEL_UART, GPIO_NUM_NC, PASSIVE_PANEL_RX_GPIO, false),
        "bridge",
        "passive panel UART");
    ESP_RETURN_ON_ERROR(
        configure_uart(MAIN_UART, GPIO_NUM_NC, PASSIVE_MAIN_RX_GPIO, false),
        "bridge",
        "passive main UART");
    esp_err_t passive_isr_result = gpio_install_isr_service(0);
    if (passive_isr_result != ESP_OK && passive_isr_result != ESP_ERR_INVALID_STATE) {
        return passive_isr_result;
    }
    ESP_RETURN_ON_ERROR(
        gpio_set_intr_type(GPIO_NUM_6, GPIO_INTR_ANYEDGE),
        "bridge",
        "passive GPIO6 edge type");
    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(
            GPIO_NUM_6, count_passive_aux_edge_isr,
            (void *)&s_passive_aux_edges[0]),
        "bridge",
        "passive GPIO6 edge counter");
    ESP_RETURN_ON_ERROR(
        gpio_set_intr_type(GPIO_NUM_7, GPIO_INTR_ANYEDGE),
        "bridge",
        "passive GPIO7 edge type");
    ESP_RETURN_ON_ERROR(
        gpio_isr_handler_add(
            GPIO_NUM_7, count_passive_aux_edge_isr,
            (void *)&s_passive_aux_edges[1]),
        "bridge",
        "passive GPIO7 edge counter");
#endif

    static link_side_t panel_runtime;
    static link_side_t main_runtime;
#if KLIMA_BRIDGE_ACTIVE
    panel_runtime = k_panel_side;
    main_runtime = k_main_side;
#if KLIMA_TAP_INJECT || KLIMA_HYBRID_BRIDGE
    panel_runtime.tx_gpio = GPIO_NUM_NC;
#endif
#elif KLIMA_PANEL_BENCH_EMULATOR
    panel_runtime = k_panel_side;
    main_runtime = k_main_side;
    main_runtime.tx_gpio = GPIO_NUM_NC;
#elif KLIMA_ALL_INPUTS_PROBE
    panel_runtime = k_panel_side;
    panel_runtime.rx_gpio = ALL_INPUTS_PANEL_RX_GPIO;
    panel_runtime.tx_gpio = GPIO_NUM_NC;
    main_runtime = k_main_side;
    main_runtime.rx_gpio = ALL_INPUTS_MAIN_RX_GPIO;
    main_runtime.tx_gpio = GPIO_NUM_NC;
#elif KLIMA_WAKE7_PROBE
    panel_runtime = k_panel_side;
    panel_runtime.rx_gpio = GPIO_NUM_5;
    panel_runtime.tx_gpio = GPIO_NUM_NC;
    main_runtime = k_main_side;
    main_runtime.rx_gpio = GPIO_NUM_4;
    main_runtime.tx_gpio = GPIO_NUM_7;
#elif KLIMA_HARDWARE_BYPASS_PROBE
    panel_runtime = k_panel_side;
    panel_runtime.rx_gpio = GPIO_NUM_7;
    panel_runtime.tx_gpio = GPIO_NUM_6;
    main_runtime = k_main_side;
    main_runtime.rx_gpio = GPIO_NUM_4;
    main_runtime.tx_gpio = GPIO_NUM_5;
#elif KLIMA_BRIDGE_MIRROR_PROBE
    panel_runtime = k_panel_side;
    panel_runtime.rx_gpio = PROBE_PANEL_RX_GPIO;
    panel_runtime.tx_gpio = GPIO_NUM_NC;
    main_runtime = k_main_side;
    main_runtime.rx_gpio = PROBE_MAIN_RX_GPIO;
    main_runtime.tx_gpio = PROBE_MAIN_TO_PANEL_GPIO;
#else
    panel_runtime = k_panel_side;
    panel_runtime.rx_gpio = PASSIVE_PANEL_RX_GPIO;
    panel_runtime.tx_gpio = GPIO_NUM_NC;
    main_runtime = k_main_side;
    main_runtime.rx_gpio = PASSIVE_MAIN_RX_GPIO;
    main_runtime.tx_gpio = GPIO_NUM_NC;
#endif

    BaseType_t created = xTaskCreate(
        stream_task, "link_panel", 4096, &panel_runtime, 16, NULL);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    created = xTaskCreate(stream_task, "link_main", 4096, &main_runtime, 16, NULL);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#if KLIMA_PANEL_BENCH_EMULATOR
    created = xTaskCreate(
        panel_bench_emulator_task, "panel_bench", 3072, NULL, 15, NULL);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#endif
#if KLIMA_BRIDGE_ACTIVE && KLIMA_HEADLESS_PROBE
    created = xTaskCreate(virtual_panel_task, "virtual_panel", 4096, NULL, 15, NULL);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#endif
#if KLIMA_BRIDGE_ACTIVE
    created = xTaskCreate(
        control_takeover_task, "control_takeover", 3072, NULL, 18, NULL);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    created = xTaskCreate(
        control_watchdog_task, "control_watchdog", 3072, NULL, 7, NULL);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#endif
    created = xTaskCreate(health_task, "bridge_health", 3072, NULL, 6, NULL);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    telemetry_publishf(
        "BRIDGE,event=started,profile=%s,baud=9600,format=8N1,panel_rx=%d,panel_tx=%d,"
        "main_rx=%d,main_tx=%d",
        bridge_service_profile_name(),
        panel_runtime.rx_gpio,
        panel_runtime.tx_gpio,
        main_runtime.rx_gpio,
        main_runtime.tx_gpio);
    return ESP_OK;
}
