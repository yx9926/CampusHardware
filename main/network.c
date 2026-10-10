#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_sntp.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "system.h"
#include "config.h"
#define WIFI_READY BIT0
#define MQTT_READY BIT1
#define TIME_SYNC BIT2
static EventGroupHandle_t signals;
static QueueHandle_t acknowledgements;
static char device_id[65], event_topic[100], telemetry_topic[100];

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        xEventGroupSetBits(signals, WIFI_READY);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
        xEventGroupClearBits(signals, WIFI_READY | MQTT_READY);
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t event = data;
    if (id == MQTT_EVENT_CONNECTED)
        xEventGroupSetBits(signals, MQTT_READY);
    if (id == MQTT_EVENT_DISCONNECTED)
        xEventGroupClearBits(signals, MQTT_READY);
    if (id == MQTT_EVENT_PUBLISHED)
    {
        int msg_id = event->msg_id;
        // 不在 MQTT 回调中写 flash；交给网络任务处理确认，避免阻塞协议任务。
        if (xQueueSend(acknowledgements, &msg_id, 0) != pdTRUE)
            ESP_LOGE("network", "PUBACK queue full; event will be replayed");
    }
}

static void time_sync(struct timeval *tv)
{
    xEventGroupSetBits(signals, TIME_SYNC);
}

static esp_mqtt_client_handle_t start_mqtt(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_CHECKIN_MQTT_URI,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.client_id = device_id,
        .credentials.username = CONFIG_CHECKIN_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_CHECKIN_MQTT_PASSWORD,
        .network.reconnect_timeout_ms = 3000,
        .network.timeout_ms = 2000,
        .session.keepalive = 30,
        .outbox.limit = 4096,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
    configASSERT(client);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(client));
    return client;
}

void network_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    signals = xEventGroupCreate();
    acknowledgements = xQueueCreate(8, sizeof(int));
    configASSERT(signals && acknowledgements);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    configASSERT(esp_netif_create_default_wifi_sta());
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    wifi_config_t wifi = {0};
    configASSERT(strlen(CONFIG_CHECKIN_WIFI_SSID) <= sizeof(wifi.sta.ssid));
    configASSERT(strlen(CONFIG_CHECKIN_WIFI_PASSWORD) <= sizeof(wifi.sta.password));
    memcpy(wifi.sta.ssid, CONFIG_CHECKIN_WIFI_SSID, strlen(CONFIG_CHECKIN_WIFI_SSID));
    memcpy(wifi.sta.password, CONFIG_CHECKIN_WIFI_PASSWORD, strlen(CONFIG_CHECKIN_WIFI_PASSWORD));
    wifi.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    if (CONFIG_CHECKIN_DEVICE_ID[0])
    {
        configASSERT(strlen(CONFIG_CHECKIN_DEVICE_ID) < sizeof(device_id));
        snprintf(device_id, sizeof(device_id), "%s", CONFIG_CHECKIN_DEVICE_ID);
    }
    else
        snprintf(device_id,
                 sizeof(device_id),
                 "AREA_%02X%02X%02X%02X%02X%02X",
                 mac[0],
                 mac[1],
                 mac[2],
                 mac[3],
                 mac[4],
                 mac[5]);
    // 标识限制字符同时防止 JSON 和 MQTT topic 注入。
    for (size_t i = 0; device_id[i]; ++i)
        configASSERT(isalnum((unsigned char)device_id[i]) || device_id[i] == '_' ||
                     device_id[i] == '-');
    snprintf(event_topic, sizeof(event_topic), "checkin/%s/events", device_id);
    snprintf(telemetry_topic, sizeof(telemetry_topic), "checkin/%s/telemetry", device_id);
    ESP_LOGI("network", "device_id=%s", device_id);
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONFIG_CHECKIN_NTP_SERVER);
    esp_sntp_set_time_sync_notification_cb(time_sync);
    esp_sntp_set_sync_interval(3600000);
    esp_sntp_init();
    esp_mqtt_client_handle_t client = start_mqtt();
    int inflight = -1;
    uint64_t sequence = 0;
    int64_t sent_at = 0, next_wifi = 0, next_telemetry = 0, next_rtc_retry = 0;
    int retry_ms = 1000;
    for (;;)
    {
        int64_t now = esp_timer_get_time();
        EventBits_t bits = xEventGroupGetBits(signals);
        if (!(bits & WIFI_READY) && now >= next_wifi && CONFIG_CHECKIN_WIFI_SSID[0])
        {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK)
                ESP_LOGW("network", "Wi-Fi connect: %s", esp_err_to_name(err));
            next_wifi = now + retry_ms * 1000LL;
            if (retry_ms < 30000)
                retry_ms = retry_ms * 2 > 30000 ? 30000 : retry_ms * 2;
        }
        else if (bits & WIFI_READY)
            retry_ms = 1000;
        if ((bits & TIME_SYNC) && now >= next_rtc_retry)
        {
            // NTP 仅用于校准；事件和遥测永远读 DS3231，不回退到系统时钟。
            time_t ntp = time(NULL);
            esp_err_t err = ntp >= 1704067200 ? ds3231_set(ntp) : ESP_ERR_INVALID_STATE;
            if (err == ESP_OK)
            {
                xEventGroupClearBits(signals, TIME_SYNC);
                ESP_LOGI("network", "DS3231 synchronized (UTC)");
            }
            else
                ESP_LOGW("network", "RTC sync failed: %s", esp_err_to_name(err));
            next_rtc_retry = now + 5000000;
        }
        int ack;
        while (xQueueReceive(acknowledgements, &ack, 0) == pdTRUE)
        {
            if (ack == inflight)
            {
                esp_err_t err = state_ack(sequence);
                if (err != ESP_OK)
                    ESP_LOGE("network", "Persist ACK failed; replay: %s", esp_err_to_name(err));
                inflight = -1;
            }
        }
        // 一个在途事件保证顺序。超时重建 client 清空旧 outbox，原 NVS 事件仍在。
        // QoS1 在断电/PUBACK 丢失时可能重复，后端以 device_id + event_id 去重。
        if (inflight >= 0 && now - sent_at > 30000000)
        {
            ESP_ERROR_CHECK(esp_mqtt_client_stop(client));
            ESP_ERROR_CHECK(esp_mqtt_client_destroy(client));
            xEventGroupClearBits(signals, MQTT_READY);
            xQueueReset(acknowledgements);
            client = start_mqtt();
            inflight = -1;
        }
        bits = xEventGroupGetBits(signals);
        if ((bits & MQTT_READY) && inflight < 0)
        {
            checkin_event_t event;
            if (state_peek(&event))
            {
                char payload[320];
                snprintf(payload,
                         sizeof(payload),
                         "{\"device_id\":\"%s\",\"event_id\":\"%" PRIu64 "\",\"timestamp\":%" PRId64
                         ",\"event_type\":\"%s\",\"headcount_after_event\":%" PRIu32 "}",
                         device_id,
                         event.sequence,
                         event.timestamp,
                         event.check_in ? "CHECK_IN" : "CHECK_OUT",
                         event.headcount);
                int msg = esp_mqtt_client_enqueue(client, event_topic, payload, 0, 1, 0, true);
                if (msg >= 0)
                {
                    inflight = msg;
                    sequence = event.sequence;
                    sent_at = now;
                }
            }
        }
        if ((bits & MQTT_READY) && now >= next_telemetry)
        {
            time_t timestamp;
            uint32_t count;
            float db;
            bool valid;
            state_snapshot(&count, &db, &valid);
            if (ds3231_read(&timestamp) == ESP_OK)
            {
                char payload[256], sound[24];
                if (valid)
                    snprintf(sound, sizeof(sound), "%.1f", db);
                else
                    snprintf(sound, sizeof(sound), "null");
                snprintf(payload,
                         sizeof(payload),
                         "{\"device_id\":\"%s\",\"timestamp\":%" PRId64 ",\"headcount\":%" PRIu32
                         ",\"decibel\":%s,\"sound_valid\":%s}",
                         device_id,
                         (int64_t)timestamp,
                         count,
                         sound,
                         valid ? "true" : "false");
                // QoS0 直接发送，不进入离线 outbox；仅网络任务受有界发送超时影响。
                if (esp_mqtt_client_publish(client, telemetry_topic, payload, 0, 0, 0) < 0)
                    ESP_LOGW("network", "Telemetry send failed; next snapshot will retry");
            }
            next_telemetry = now + TELEMETRY_MS * 1000LL;
        }
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
