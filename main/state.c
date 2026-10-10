#include <string.h>
#include <limits.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "system.h"
#include "config.h"

typedef struct
{
    uint32_t version, count, headcount;
    uint64_t next_sequence;
    checkin_event_t events[JOURNAL_CAPACITY];
} journal_t;

static journal_t journal, candidate;
static SemaphoreHandle_t mutex;
static nvs_handle_t nvs;
static float decibel;
static bool sound_valid;

// 整个快照使用单个 NVS blob：事件与人数在同一次原子更新中提交。
// 写入失败时保留旧内存状态；PUBACK 前绝不从队列删除。
static esp_err_t save(void)
{
    esp_err_t err = nvs_set_blob(nvs, "journal", &candidate, sizeof(candidate));
    if (err == ESP_OK)
        err = nvs_commit(nvs);
    if (err == ESP_OK)
        journal = candidate;
    return err;
}

void state_init(void)
{
    mutex = xSemaphoreCreateMutex();
    configASSERT(mutex);
    ESP_ERROR_CHECK(nvs_open("checkin", NVS_READWRITE, &nvs));
    size_t size = sizeof(journal);
    esp_err_t err = nvs_get_blob(nvs, "journal", &journal, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        journal.version = 1;
        journal.next_sequence = 1;
    }
    else
    {
        ESP_ERROR_CHECK(err);
        ESP_ERROR_CHECK(size == sizeof(journal) && journal.version == 1 &&
                                journal.count <= JOURNAL_CAPACITY && journal.next_sequence > 0
                            ? ESP_OK
                            : ESP_ERR_INVALID_STATE);
        for (uint32_t i = 0; i < journal.count; ++i)
        {
            const checkin_event_t *event = &journal.events[i];
            ESP_ERROR_CHECK(
                event->sequence > 0 && event->sequence < journal.next_sequence &&
                        event->timestamp >= 946684800 && event->check_in <= 1 &&
                        (i == 0 || event->sequence == journal.events[i - 1].sequence + 1)
                    ? ESP_OK
                    : ESP_ERR_INVALID_STATE);
        }
        if (journal.count)
            ESP_ERROR_CHECK(journal.events[journal.count - 1].headcount == journal.headcount
                                ? ESP_OK
                                : ESP_ERR_INVALID_STATE);
    }
}

esp_err_t state_record(bool check_in, time_t timestamp)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (journal.count < JOURNAL_CAPACITY && journal.next_sequence != UINT64_MAX &&
        (!check_in || journal.headcount < UINT32_MAX))
    {
        candidate = journal;
        if (check_in)
            candidate.headcount++;
        else if (candidate.headcount)
            candidate.headcount--;
        candidate.events[candidate.count++] =
            (checkin_event_t){.sequence = candidate.next_sequence++,
                              .timestamp = timestamp,
                              .headcount = candidate.headcount,
                              .check_in = check_in};
        err = save();
    }
    xSemaphoreGive(mutex);
    return err;
}

bool state_peek(checkin_event_t *event)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool found = journal.count > 0;
    if (found)
        *event = journal.events[0];
    xSemaphoreGive(mutex);
    return found;
}

esp_err_t state_ack(uint64_t sequence)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (journal.count && journal.events[0].sequence == sequence)
    {
        candidate = journal;
        candidate.count--;
        memmove(candidate.events, candidate.events + 1, candidate.count * sizeof(checkin_event_t));
        err = save();
    }
    xSemaphoreGive(mutex);
    return err;
}

void state_sound(float db, bool valid)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    decibel = db;
    sound_valid = valid;
    xSemaphoreGive(mutex);
}

void state_snapshot(uint32_t *headcount, float *db, bool *valid)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    *headcount = journal.headcount;
    *db = decibel;
    *valid = sound_valid;
    xSemaphoreGive(mutex);
}
