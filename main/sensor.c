#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "system.h"
#include "config.h"

void sensor_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    adc_oneshot_unit_handle_t adc;
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit, &adc));
    adc_oneshot_chan_cfg_t channel = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, ADC_CHANNEL, &channel));
    adc_cali_handle_t calibration = NULL;
    adc_cali_curve_fitting_config_t cfg = {.unit_id = ADC_UNIT_1,
                                           .chan = ADC_CHANNEL,
                                           .atten = ADC_ATTEN_DB_12,
                                           .bitwidth = ADC_BITWIDTH_12};
    if (adc_cali_create_scheme_curve_fitting(&cfg, &calibration) != ESP_OK)
        ESP_LOGW("sound", "ADC calibration unavailable; using approximate voltage");
    for (;;)
    {
        int min = 4095, max = 0, samples = 0, failures = 0, clipped = 0;
        int64_t end = esp_timer_get_time() + SAMPLE_WINDOW_US;
        while (esp_timer_get_time() < end)
        {
            int value;
            if (adc_oneshot_read(adc, ADC_CHANNEL, &value) == ESP_OK && value >= 0 && value <= 4095)
            {
                if (value < min)
                    min = value;
                if (value > max)
                    max = value;
                if (value <= 5 || value >= 4090)
                    clipped++;
                samples++;
            }
            else
                failures++;
            esp_rom_delay_us(SAMPLE_INTERVAL_US);
            if ((samples + failures) % 32 == 0)
                vTaskDelay(1);
        }
        bool valid = samples >= 100 && failures < samples / 10 && clipped == 0;
        int low_mv = 0, high_mv = 0;
        if (valid && calibration)
        {
            valid = adc_cali_raw_to_voltage(calibration, min, &low_mv) == ESP_OK &&
                    adc_cali_raw_to_voltage(calibration, max, &high_mv) == ESP_OK;
        }
        else if (valid)
        {
            low_mv = min * 3300 / 4095;
            high_mv = max * 3300 / 4095;
        }
        // 50ms 峰峰值去除直流偏置。MAX9814 自带 AGC，必须参考声源校准；
        // 近似 dB 不能作为合规声级计读数。ADC 异常、削顶时标记无效。
        float db = 0;
        if (valid)
        {
            float pp = fmaxf(1.0f, high_mv - low_mv);
            db = 94.0f + 20.0f * log10f(pp / CONFIG_CHECKIN_DB_REFERENCE_MV);
            db = fminf(120.0f, fmaxf(0.0f, db));
        }
        state_sound(db, valid);
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
