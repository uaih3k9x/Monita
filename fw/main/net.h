// 网络：WiFi STA + 轮询 face.json → mood_update
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

extern volatile bool g_net;   // WiFi 是否拿到 IP

void wifi_start(void);        // 起 WiFi STA（凭据见 wifi_secret.h）
void poll_task(void *arg);    // 每 4s GET face.json → mood_update
int  net_fetch(const char *url, uint8_t **out);  // 下载文件到 PSRAM（*out 调用方 free），返回字节数/-1
