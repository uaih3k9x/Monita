// OTA 自更新（手动 esp_ota，纯 HTTP 流式下载）—— 终结 USB 烧录
#pragma once

void ota_task(void *arg);   // 后台任务：每 15s 查 monita.ver，>本机版本就拉新固件自升重启
