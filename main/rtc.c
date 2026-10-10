#include <stdlib.h>
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "system.h"
#include "config.h"
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t device;
static SemaphoreHandle_t mutex;

static int bcd(uint8_t v)
{
    return (v & 15) <= 9 && (v >> 4) <= 9 ? (v >> 4) * 10 + (v & 15) : -1;
}

static uint8_t encode(int v)
{
    return ((v / 10) << 4) | (v % 10);
}

static esp_err_t read_reg(uint8_t reg, uint8_t *data, size_t size)
{
    esp_err_t err = i2c_master_transmit_receive(device, &reg, 1, data, size, I2C_TIMEOUT_MS);
    if (err != ESP_OK)
    {
        // 有界超时后重置控制器并重试一次；持续硬件故障时返回，绝不死等。
        i2c_master_bus_reset(bus);
        err = i2c_master_transmit_receive(device, &reg, 1, data, size, I2C_TIMEOUT_MS);
    }
    return err;
}

void ds3231_init(void)
{
    setenv("TZ", "UTC0", 1);
    tzset();
    mutex = xSemaphoreCreateMutex();
    configASSERT(mutex);
    i2c_master_bus_config_t cfg = {.i2c_port = I2C_NUM_0,
                                   .sda_io_num = PIN_SDA,
                                   .scl_io_num = PIN_SCL,
                                   .clk_source = I2C_CLK_SRC_DEFAULT,
                                   .glitch_ignore_cnt = 7,
                                   .flags.enable_internal_pullup = true};
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &bus));
    i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                               .device_address = RTC_ADDRESS,
                               .scl_speed_hz = 100000};
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev, &device));
}

esp_err_t ds3231_read(time_t *timestamp)
{
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(500)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    uint8_t raw[7], status;
    esp_err_t err = read_reg(0x0f, &status, 1);
    if (err == ESP_OK && (status & 0x80))
        err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK)
        err = read_reg(0, raw, sizeof(raw));
    if (err == ESP_OK)
    {
        int hour = bcd(raw[2] & ((raw[2] & 0x40) ? 0x1f : 0x3f));
        if (raw[2] & 0x40)
        {
            if (hour < 1 || hour > 12)
                hour = -1;
            else
                hour = hour % 12 + ((raw[2] & 0x20) ? 12 : 0);
        }
        struct tm t = {.tm_sec = bcd(raw[0] & 0x7f),
                       .tm_min = bcd(raw[1] & 0x7f),
                       .tm_hour = hour,
                       .tm_mday = bcd(raw[4] & 0x3f),
                       .tm_mon = bcd(raw[5] & 0x1f) - 1,
                       .tm_year = bcd(raw[6]) + 100 + ((raw[5] & 0x80) ? 100 : 0),
                       .tm_isdst = 0};
        struct tm original = t;
        // mktime 自动归一化非法日期，回比防止接受 2 月 31 日等异常。
        if (t.tm_sec < 0 || t.tm_sec > 59 || t.tm_min < 0 || t.tm_min > 59 || hour < 0 ||
            hour > 23 || t.tm_mday < 1 || t.tm_mon < 0 || t.tm_mon > 11 || bcd(raw[6]) < 0)
            err = ESP_ERR_INVALID_RESPONSE;
        else
        {
            time_t value = mktime(&t);
            if (value < 946684800 || t.tm_mday != original.tm_mday || t.tm_mon != original.tm_mon ||
                t.tm_year != original.tm_year)
                err = ESP_ERR_INVALID_RESPONSE;
            else
                *timestamp = value;
        }
    }
    xSemaphoreGive(mutex);
    return err;
}

esp_err_t ds3231_set(time_t timestamp)
{
    struct tm t;
    gmtime_r(&timestamp, &t);
    if (t.tm_year < 100 || t.tm_year > 299)
        return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(500)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    uint8_t data[] = {0,
                      encode(t.tm_sec),
                      encode(t.tm_min),
                      encode(t.tm_hour),
                      encode(t.tm_wday + 1),
                      encode(t.tm_mday),
                      encode(t.tm_mon + 1) | (t.tm_year >= 200 ? 0x80 : 0),
                      encode(t.tm_year % 100)};
    uint8_t control = 0, status = 0;
    esp_err_t err = read_reg(0x0e, &control, 1);
    uint8_t ctrl[] = {0x0e, control & 0x7f};
    // 保持电池供电时振荡；完整写入时间后才清除 OSF。
    if (err == ESP_OK)
        err = i2c_master_transmit(device, ctrl, 2, I2C_TIMEOUT_MS);
    if (err == ESP_OK)
        err = i2c_master_transmit(device, data, sizeof(data), I2C_TIMEOUT_MS);
    if (err == ESP_OK)
        err = read_reg(0x0f, &status, 1);
    if (err == ESP_OK)
    {
        uint8_t clear[] = {0x0f, status & 0x7f};
        err = i2c_master_transmit(device, clear, 2, I2C_TIMEOUT_MS);
    }
    if (err != ESP_OK)
        i2c_master_bus_reset(bus);
    xSemaphoreGive(mutex);
    return err;
}
