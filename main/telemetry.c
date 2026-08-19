#include "telemetry.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TELEMETRY_RING_ENTRIES 96U

static telemetry_entry_t s_entries[TELEMETRY_RING_ENTRIES];
static SemaphoreHandle_t s_lock;
static uint64_t s_latest_sequence;

esp_err_t telemetry_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

void telemetry_publishf(const char *format, ...)
{
    char line[sizeof(s_entries[0].text)];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    const uint64_t sequence = ++s_latest_sequence;
    telemetry_entry_t *entry = &s_entries[(sequence - 1U) % TELEMETRY_RING_ENTRIES];
    entry->sequence = sequence;
    strlcpy(entry->text, line, sizeof(entry->text));
    xSemaphoreGive(s_lock);
}

uint64_t telemetry_latest_sequence(void)
{
    uint64_t result = 0;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        result = s_latest_sequence;
        xSemaphoreGive(s_lock);
    }
    return result;
}

bool telemetry_get_after(uint64_t after, telemetry_entry_t *entry)
{
    if (!entry || !s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }

    const uint64_t latest = s_latest_sequence;
    const uint64_t oldest = latest > TELEMETRY_RING_ENTRIES
        ? latest - TELEMETRY_RING_ENTRIES + 1U
        : (latest ? 1U : 0U);
    uint64_t wanted = after + 1U;
    if (wanted < oldest) {
        wanted = oldest;
    }

    const bool found = wanted != 0U && wanted <= latest;
    if (found) {
        *entry = s_entries[(wanted - 1U) % TELEMETRY_RING_ENTRIES];
    }
    xSemaphoreGive(s_lock);
    return found;
}
