#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

typedef struct
{
    uint64_t sequence;
    int64_t timestamp;
    uint32_t headcount;
    uint8_t check_in;
} checkin_event_t;

void state_init(void);
esp_err_t state_record(bool check_in, time_t timestamp);
bool state_peek(checkin_event_t *event);
esp_err_t state_ack(uint64_t sequence);
void state_sound(float db, bool valid);
void state_snapshot(uint32_t *headcount, float *db, bool *valid);
void ds3231_init(void);
esp_err_t ds3231_read(time_t *timestamp);
esp_err_t ds3231_set(time_t timestamp);
void sensor_task(void *arg);
void buttons_task(void *arg);
void network_task(void *arg);
