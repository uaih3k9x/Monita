// 网络状态（WiFi）—— 后续 net.c 提取后这里再补 wifi_start/poll_task
#pragma once
#include <stdbool.h>

extern volatile bool g_net;   // WiFi 是否拿到 IP
