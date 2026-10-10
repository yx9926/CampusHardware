#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_task_wdt.h"
#include "system.h"
#include "config.h"
#include "driver/gpio.h"

void app_main(void)
{
    const int pins[] = {PIN_SDA, PIN_SCL, PIN_IN, PIN_OUT, ADC_CHANNEL + 1};
    for (int i = 0; i < 5; ++i)
    {
        configASSERT(GPIO_IS_VALID_GPIO(pins[i]));
        for (int j = 0; j < i; ++j)
            configASSERT(pins[i] != pins[j]);
    }
    configASSERT(GPIO_IS_VALID_OUTPUT_GPIO(PIN_SDA) && GPIO_IS_VALID_OUTPUT_GPIO(PIN_SCL));
    // NVS 异常时停止启动，禁止自动擦除尚未同步的签到记录。
    ESP_ERROR_CHECK(nvs_flash_init());
    state_init();
    ds3231_init();
    esp_task_wdt_config_t wdt = {.timeout_ms = WDT_TIMEOUT_MS,
                                 .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
                                 .trigger_panic = true};
    esp_err_t err = esp_task_wdt_reconfigure(&wdt);
    if (err == ESP_ERR_INVALID_STATE)
        err = esp_task_wdt_init(&wdt);
    ESP_ERROR_CHECK(err);
    configASSERT(xTaskCreate(sensor_task, "sound", 4096, NULL, 4, NULL) == pdPASS);
    configASSERT(xTaskCreate(buttons_task, "buttons", 4096, NULL, 5, NULL) == pdPASS);
    configASSERT(xTaskCreate(network_task, "network", 6144, NULL, 3, NULL) == pdPASS);
}
