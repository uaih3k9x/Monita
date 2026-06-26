#include "ota.h"
#include "version.h"
#include "net.h"                 // g_net
#include "power.h"               // power_read（顺便刷电量）
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_system.h"          // esp_restart
#include "esp_log.h"
#include <stdlib.h>

#define OTA_VER_URL  "http://192.168.2.254/monita.ver"       // 内容是个数字
#define OTA_BIN_URL  "http://192.168.2.254/monita-fw.bin"

static const char *TAG = "ota";

static int http_get_int(const char *url, int def)
{
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 4000 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    int v = def;
    if (c && esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        char b[16] = {0}; int total = 0, n;
        while (total < (int)sizeof(b) - 1 &&
               (n = esp_http_client_read(c, b + total, sizeof(b) - 1 - total)) > 0) total += n;
        if (total > 0) { b[total] = 0; v = atoi(b); }
        esp_http_client_close(c);
    }
    if (c) esp_http_client_cleanup(c);
    return v;
}

// 手动 OTA：HTTP 流式下载 → 写入备用分区（纯 HTTP，无 TLS 假设）
static esp_err_t do_ota(const char *url)
{
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 30000, .keep_alive_enable = true };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;
    if (esp_http_client_open(c, 0) != ESP_OK) { esp_http_client_cleanup(c); return ESP_FAIL; }
    esp_http_client_fetch_headers(c);

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h = 0;
    esp_err_t err = part ? esp_ota_begin(part, OTA_SIZE_UNKNOWN, &h) : ESP_FAIL;
    if (err != ESP_OK) { esp_http_client_close(c); esp_http_client_cleanup(c); return ESP_FAIL; }

    char *buf = malloc(4096);
    int n, written = 0;
    while ((n = esp_http_client_read(c, buf, 4096)) > 0) {
        if (esp_ota_write(h, buf, n) != ESP_OK) { err = ESP_FAIL; break; }
        written += n;
    }
    free(buf);
    if (n < 0) err = ESP_FAIL;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (err == ESP_OK && esp_ota_end(h) == ESP_OK && esp_ota_set_boot_partition(part) == ESP_OK) {
        ESP_LOGW(TAG, "OTA 写入 %d 字节", written);
        return ESP_OK;
    }
    esp_ota_abort(h);
    return ESP_FAIL;
}

void ota_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        if (!g_net) continue;
        power_read();                                     // 顺便刷新电量
        int remote = http_get_int(OTA_VER_URL, -1);
        ESP_LOGI(TAG, "OTA 检查：远程 ver=%d，本机 v%d", remote, FW_VERSION);
        if (remote > FW_VERSION) {
            ESP_LOGW(TAG, "发现新固件 v%d（本机 v%d）→ OTA…", remote, FW_VERSION);
            if (do_ota(OTA_BIN_URL) == ESP_OK) { ESP_LOGW(TAG, "OTA 成功，重启"); esp_restart(); }
            else ESP_LOGE(TAG, "OTA 失败");
        }
    }
}
