#include "net.h"
#include "mood.h"                // mood_update
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include "wifi_secret.h"         // WIFI_SSID / WIFI_PASS（本地文件，不提交）

#define FACE_URL  "http://192.168.2.254/face.json"

static const char *TAG = "net";

volatile bool g_net = false;

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) { g_net = false; esp_wifi_connect(); }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) { g_net = true; ESP_LOGI(TAG, "WiFi 已连，拿到 IP"); }
}

void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL, NULL));
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "连 %s → 轮询 %s 驱动表情", WIFI_SSID, FACE_URL);
}

// 每 4s 拉 face.json → 交给 mood_update 驱动表情
void poll_task(void *arg)
{
    while (true) {
        if (g_net) {
            esp_http_client_config_t cfg = { .url = FACE_URL, .timeout_ms = 4000 };
            esp_http_client_handle_t c = esp_http_client_init(&cfg);
            if (c && esp_http_client_open(c, 0) == ESP_OK) {
                esp_http_client_fetch_headers(c);
                char body[512]; int total = 0, n;
                while ((n = esp_http_client_read(c, body + total, sizeof(body) - 1 - total)) > 0) {
                    total += n;
                    if (total >= (int)sizeof(body) - 1) break;
                }
                body[total] = 0;
                if (total > 0) {
                    cJSON *j = cJSON_Parse(body);
                    if (j) { mood_update(j); cJSON_Delete(j); }
                }
                esp_http_client_close(c);
            }
            if (c) esp_http_client_cleanup(c);
        }
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}
