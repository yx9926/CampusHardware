#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "system.h"
#include "config.h"
static TaskHandle_t button_handle;

static void IRAM_ATTR button_isr(void *arg)
{
    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(button_handle, &wake);
    if (wake)
        portYIELD_FROM_ISR();
}

void buttons_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    button_handle = xTaskGetCurrentTaskHandle();
    gpio_config_t cfg = {.pin_bit_mask = (1ULL << PIN_IN) | (1ULL << PIN_OUT),
                         .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE,
                         .intr_type = GPIO_INTR_ANYEDGE};
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_IN, button_isr, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_OUT, button_isr, NULL));
    int pins[] = {PIN_IN, PIN_OUT};
    int stable[] = {1, 1}, candidate[] = {1, 1};
    int64_t changed[] = {0, 0};
    for (;;)
    {
        // ISR 只唤醒；定期复查使通知合并也不会漏掉稳定状态。
        // 按下和释放都稳定 50ms；长按只记一次，释放后重新武装。
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        bool pressed[2] = {false, false};
        for (int i = 0; i < 2; ++i)
        {
            int64_t now = esp_timer_get_time();
            int level = gpio_get_level(pins[i]);
            if (level != candidate[i])
            {
                candidate[i] = level;
                changed[i] = now;
            }
            if (level != stable[i] && now - changed[i] >= DEBOUNCE_MS * 1000LL)
            {
                stable[i] = level;
                pressed[i] = level == 0;
            }
        }
        // 先采集两个按键的稳定状态，再处理可能需要 flash/I²C 的业务。
        for (int i = 0; i < 2; ++i)
        {
            if (pressed[i])
            {
                time_t timestamp;
                esp_err_t err = ds3231_read(&timestamp);
                if (err == ESP_OK)
                    err = state_record(i == 0, timestamp);
                if (err != ESP_OK)
                    ESP_LOGE("buttons", "Event REJECTED (RTC/storage): %s", esp_err_to_name(err));
                else
                    ESP_LOGI("buttons", "%s recorded", i == 0 ? "CHECK_IN" : "CHECK_OUT");
            }
        }
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        vTaskDelay(1);
    }
}
