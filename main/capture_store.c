#include "capture_store.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CAPTURE_PARTITION_LABEL "capture"
#define CAPTURE_PARTITION_SUBTYPE 0x40
#define CAPTURE_USABLE_LIMIT 0x1C000U
#define CAPTURE_SECTOR_SIZE 4096U
#define CAPTURE_RECORD_SIZE 128U
#define CAPTURE_MAX_RECORDS (CAPTURE_USABLE_LIMIT / CAPTURE_RECORD_SIZE)
#define CAPTURE_QUEUE_LENGTH 24U
#define CAPTURE_MAGIC 0x3150434BU /* little-endian ASCII KCP1 */

static const char *TAG = "capture_store";

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    int64_t timestamp_us;
    uint8_t type;
    uint8_t direction;
    uint8_t kind;
    uint8_t length;
    uint8_t data[CAPTURE_STORE_DATA_MAX];
    uint32_t crc32;
} capture_flash_record_t;

_Static_assert(sizeof(capture_flash_record_t) == CAPTURE_RECORD_SIZE, "capture record must be 128 bytes");

typedef struct {
    int64_t timestamp_us;
    uint8_t type;
    uint8_t direction;
    uint8_t kind;
    uint8_t length;
    uint8_t data[CAPTURE_STORE_DATA_MAX];
} capture_request_t;

static const esp_partition_t *s_partition;
static size_t s_usable_size;
static size_t s_slot_count;
static size_t s_next_offset;
static uint32_t s_next_sequence = 1U;
static uint32_t s_slot_sequences[CAPTURE_MAX_RECORDS];
static capture_store_info_t s_info;
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;

static uint32_t crc32_bytes(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

static bool record_valid(const capture_flash_record_t *record)
{
    return record && record->magic == CAPTURE_MAGIC &&
        record->sequence != 0U && record->length <= CAPTURE_STORE_DATA_MAX &&
        record->type >= CAPTURE_RECORD_FRAME && record->type <= CAPTURE_RECORD_SYSTEM &&
        record->crc32 == crc32_bytes((const uint8_t *)record, offsetof(capture_flash_record_t, crc32));
}

static bool record_erased(const capture_flash_record_t *record)
{
    const uint8_t *bytes = (const uint8_t *)record;
    for (size_t i = 0; i < sizeof(*record); ++i) {
        if (bytes[i] != 0xFFU) {
            return false;
        }
    }
    return true;
}

static void refresh_info_locked(void)
{
    uint32_t oldest = 0U;
    uint32_t latest = 0U;
    uint32_t count = 0U;
    for (size_t slot = 0; slot < s_slot_count; ++slot) {
        const uint32_t sequence = s_slot_sequences[slot];
        if (sequence == 0U) {
            continue;
        }
        ++count;
        if (oldest == 0U || sequence < oldest) {
            oldest = sequence;
        }
        if (sequence > latest) {
            latest = sequence;
        }
    }
    s_info.valid_records = count;
    s_info.oldest_sequence = oldest;
    s_info.latest_sequence = latest;
}

static void forget_sector_locked(size_t sector_offset)
{
    const size_t first_slot = sector_offset / CAPTURE_RECORD_SIZE;
    const size_t slots = CAPTURE_SECTOR_SIZE / CAPTURE_RECORD_SIZE;
    for (size_t i = 0; i < slots && first_slot + i < s_slot_count; ++i) {
        s_slot_sequences[first_slot + i] = 0U;
    }
}

static esp_err_t prepare_next_slot_locked(void)
{
    capture_flash_record_t existing;
    ESP_RETURN_ON_ERROR(
        esp_partition_read(s_partition, s_next_offset, &existing, sizeof(existing)),
        TAG,
        "read next slot");

    if ((s_next_offset % CAPTURE_SECTOR_SIZE) != 0U && !record_erased(&existing)) {
        s_next_offset = ((s_next_offset / CAPTURE_SECTOR_SIZE) + 1U) * CAPTURE_SECTOR_SIZE;
        if (s_next_offset >= s_usable_size) {
            s_next_offset = 0U;
        }
    }
    if ((s_next_offset % CAPTURE_SECTOR_SIZE) == 0U) {
        ESP_RETURN_ON_ERROR(
            esp_partition_erase_range(s_partition, s_next_offset, CAPTURE_SECTOR_SIZE),
            TAG,
            "erase capture sector");
        forget_sector_locked(s_next_offset);
    }
    return ESP_OK;
}

static void write_request(const capture_request_t *request)
{
    if (!request || !s_partition || !s_lock || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    esp_err_t err = prepare_next_slot_locked();
    if (err == ESP_OK) {
        capture_flash_record_t record = {
            .magic = CAPTURE_MAGIC,
            .sequence = s_next_sequence++,
            .timestamp_us = request->timestamp_us,
            .type = request->type,
            .direction = request->direction,
            .kind = request->kind,
            .length = request->length,
        };
        memcpy(record.data, request->data, request->length);
        record.crc32 = crc32_bytes(
            (const uint8_t *)&record,
            offsetof(capture_flash_record_t, crc32));
        err = esp_partition_write(s_partition, s_next_offset, &record, sizeof(record));
        if (err == ESP_OK) {
            const size_t slot = s_next_offset / CAPTURE_RECORD_SIZE;
            s_slot_sequences[slot] = record.sequence;
            s_next_offset += CAPTURE_RECORD_SIZE;
            if (s_next_offset >= s_usable_size) {
                s_next_offset = 0U;
            }
            refresh_info_locked();
        }
    }
    if (err != ESP_OK) {
        ++s_info.write_errors;
        ESP_LOGW(TAG, "capture write failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
}

static void capture_task(void *arg)
{
    (void)arg;
    capture_request_t request;
    while (true) {
        if (xQueueReceive(s_queue, &request, portMAX_DELAY) == pdTRUE) {
            write_request(&request);
        }
    }
}

static esp_err_t enqueue_record(
    capture_record_type_t type,
    capture_direction_t direction,
    capture_kind_t kind,
    const void *data,
    size_t length,
    TickType_t timeout)
{
    if (!s_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!data || length == 0U || length > CAPTURE_STORE_DATA_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    capture_request_t request = {
        .timestamp_us = esp_timer_get_time(),
        .type = (uint8_t)type,
        .direction = (uint8_t)direction,
        .kind = (uint8_t)kind,
        .length = (uint8_t)length,
    };
    memcpy(request.data, data, length);
    if (xQueueSend(s_queue, &request, timeout) != pdTRUE) {
        __atomic_fetch_add(&s_info.dropped_records, 1U, __ATOMIC_RELAXED);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t capture_store_init(void)
{
    s_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        CAPTURE_PARTITION_SUBTYPE,
        CAPTURE_PARTITION_LABEL);
    if (!s_partition) {
        ESP_LOGW(TAG, "capture partition not found; persistent capture disabled");
        return ESP_OK;
    }
    s_usable_size = s_partition->size < CAPTURE_USABLE_LIMIT
        ? s_partition->size
        : CAPTURE_USABLE_LIMIT;
    s_usable_size -= s_usable_size % CAPTURE_SECTOR_SIZE;
    s_slot_count = s_usable_size / CAPTURE_RECORD_SIZE;
    if (s_slot_count == 0U || s_slot_count > CAPTURE_MAX_RECORDS) {
        return ESP_ERR_INVALID_SIZE;
    }
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(CAPTURE_QUEUE_LENGTH, sizeof(capture_request_t));
    if (!s_lock || !s_queue) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t latest_sequence = 0U;
    size_t latest_offset = 0U;
    for (size_t slot = 0; slot < s_slot_count; ++slot) {
        capture_flash_record_t record;
        if (esp_partition_read(
                s_partition,
                slot * CAPTURE_RECORD_SIZE,
                &record,
                sizeof(record)) == ESP_OK && record_valid(&record)) {
            s_slot_sequences[slot] = record.sequence;
            if (record.sequence > latest_sequence) {
                latest_sequence = record.sequence;
                latest_offset = slot * CAPTURE_RECORD_SIZE;
            }
        }
    }
    s_next_sequence = latest_sequence == UINT32_MAX ? 1U : latest_sequence + 1U;
    s_next_offset = latest_sequence == 0U ? 0U : latest_offset + CAPTURE_RECORD_SIZE;
    if (s_next_offset >= s_usable_size) {
        s_next_offset = 0U;
    }
    s_info.available = true;
    s_info.capacity_records = (uint32_t)s_slot_count;
    refresh_info_locked();

    if (xTaskCreate(capture_task, "capture_store", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    char boot[CAPTURE_STORE_DATA_MAX];
    const int length = snprintf(
        boot,
        sizeof(boot),
        "version=%s reset_reason=%d partition=%s",
        app->version,
        (int)esp_reset_reason(),
        running ? running->label : "unknown");
    if (length > 0) {
        const size_t stored_length = (size_t)length < sizeof(boot)
            ? (size_t)length
            : sizeof(boot) - 1U;
        (void)enqueue_record(
            CAPTURE_RECORD_BOOT,
            CAPTURE_DIRECTION_NONE,
            CAPTURE_KIND_NONE,
            boot,
            stored_length,
            0);
    }
    ESP_LOGI(TAG, "persistent capture ready records=%u", (unsigned)s_slot_count);
    return ESP_OK;
}

void capture_store_record_frame(
    capture_direction_t direction,
    capture_kind_t kind,
    const uint8_t *data,
    size_t length)
{
    (void)enqueue_record(CAPTURE_RECORD_FRAME, direction, kind, data, length, 0);
}

esp_err_t capture_store_record_marker(const char *label)
{
    if (!label) {
        return ESP_ERR_INVALID_ARG;
    }
    return enqueue_record(
        CAPTURE_RECORD_MARKER,
        CAPTURE_DIRECTION_NONE,
        CAPTURE_KIND_NONE,
        label,
        strlen(label),
        pdMS_TO_TICKS(100));
}

esp_err_t capture_store_record_system(const char *event)
{
    if (!event) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t length = strlen(event);
    if (length == 0U || length > CAPTURE_STORE_DATA_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return enqueue_record(
        CAPTURE_RECORD_SYSTEM,
        CAPTURE_DIRECTION_NONE,
        CAPTURE_KIND_NONE,
        event,
        length,
        0);
}

void capture_store_get_info(capture_store_info_t *info)
{
    if (!info) {
        return;
    }
    memset(info, 0, sizeof(*info));
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        *info = s_info;
        info->dropped_records = __atomic_load_n(&s_info.dropped_records, __ATOMIC_RELAXED);
        xSemaphoreGive(s_lock);
    }
}

bool capture_store_get_after(uint32_t after, capture_store_record_t *record)
{
    if (!record || !s_partition || !s_lock ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    uint32_t wanted = 0U;
    size_t wanted_slot = 0U;
    for (size_t slot = 0; slot < s_slot_count; ++slot) {
        const uint32_t sequence = s_slot_sequences[slot];
        if (sequence > after && (wanted == 0U || sequence < wanted)) {
            wanted = sequence;
            wanted_slot = slot;
        }
    }
    bool found = false;
    if (wanted != 0U) {
        capture_flash_record_t stored;
        if (esp_partition_read(
                s_partition,
                wanted_slot * CAPTURE_RECORD_SIZE,
                &stored,
                sizeof(stored)) == ESP_OK && record_valid(&stored)) {
            *record = (capture_store_record_t){
                .sequence = stored.sequence,
                .timestamp_us = stored.timestamp_us,
                .type = (capture_record_type_t)stored.type,
                .direction = (capture_direction_t)stored.direction,
                .kind = (capture_kind_t)stored.kind,
                .length = stored.length,
            };
            memcpy(record->data, stored.data, stored.length);
            found = true;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}
