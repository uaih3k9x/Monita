# 模拟太 · Monita

> 名字取自《逆转裁判》希月心音胸前那台 **モニ太（模拟太）**——一块会随主人情绪变色、
> 偶尔还替她把心里话说出来的小电脑。这只是它的现实版。日常我们也叫它「小圆脸」。

一只骑在 5G CPE 上、会根据**真实网络状态**卖萌的「无口萌」表情屏。

一块 ESP32-S3 AMOLED 圆屏，显示一张没有嘴、全靠两只发光眼睛说话的脸。它通过 WiFi
读取路由器（CPE）的蜂窝网络指标，把信号好坏 / 忙不忙 / 是否掉线，实时映射成六种情绪。

> 不是仪表盘，是陪伴感。会变的 → 脸做反应；常驻数值 → 以后翻页查。

## 效果

- 浏览器预览版：直接打开 [`index.html`](index.html)（纯 Canvas 画的无口萌脸 + 六种表情按钮）。
- 设备版：`fw/` 下的 ESP-IDF 固件，跑在真 AMOLED 上。

## 硬件

| | |
|---|---|
| 主控屏 | Waveshare **ESP32-S3-Touch-AMOLED-1.75**（CO5300 466×466 圆形 AMOLED，QSPI；8MB PSRAM / 32MB Flash） |
| 数据源 | OpenWRT 路由 / NRadio CPE（5G 模组），LAN 内提供 `face.json` |

## 工作原理

```
CPE: ubus(infocd cpestatus) ──每3s──▶ /www/face.json  (uhttpd :80)
                                            │  HTTP GET（每 4s）
ESP32-S3 ── WiFi ──────────────────────────┘
   └─ 解析 JSON → 情绪两轴映射 → setMood → 渲染脸
```

- **情绪两轴**：信号质量（RSRP/SINR）决定「舒不舒服」的底噪心情；吞吐（dl/ul）决定「忙不忙」。
- **六种表情**：`happy / grin / busy / surprised / offline / sleepy`，眼形 + 腮红/汗/泪 + 中文台词气泡。
- **渲染**：纯手写，无 LVGL。圆角盒 / 抛物线弧 / 折线 SDF 画眼睛，分层加性合成上光晕与配件，
  中文气泡用离线烤的 GB2312 点阵字库（见 `tools/genfont.py`）。

## 目录

```
index.html              浏览器预览（设计原型）
fw/                     ESP-IDF 固件
  main/monita_main.c    全部逻辑：显示驱动 / 六态 / 动画 / WiFi / 取数
  main/font_cjk.h       生成的 GB2312 24×24 点阵字库
  sdkconfig.defaults    板级配置（32MB Flash + Octal PSRAM + -O2）
  partitions.csv
tools/genfont.py        字库烤制脚本（macOS + PIL）
```

## 构建 / 烧录

需要 [ESP-IDF v5.4](https://docs.espressif.com/projects/esp-idf/)（target `esp32s3`）。

```bash
cd fw
cp main/wifi_secret.h.example main/wifi_secret.h   # 填入你的 WiFi
# 改 main/monita_main.c 里的 FACE_URL 指向你的数据源
idf.py set-target esp32s3
idf.py -p <PORT> flash monitor
```

> 数据出口：在 CPE 上放一个定时把蜂窝指标写成 JSON 的脚本（`/www/face.json`，字段见
> `map_mood()`），ESP32 直接 `GET http://<网关>/face.json` 即可。

## 重新烤字库（可选）

`font_cjk.h` 已随仓库提供。要改字号/字符集，在 macOS 上：

```bash
python3 tools/genfont.py     # 需要 Pillow + 系统中文字体
```

## 进度

- [x] 浏览器原型（六态 + 待机动画）
- [x] 点屏（CO5300 QSPI 驱动）
- [x] 无口萌脸 + 眨眼 / 呼吸 / 瞟眼
- [x] 六种表情 + 腮红 / 汗 / 泪 + 中文气泡
- [x] WiFi 连网 + 轮询 `face.json` + 数据驱动表情
- [x] OTA 自更新（WiFi 拉固件，build → 拷 bin 到路由器 → 板子自升，告别 USB）
- [x] 电量读取（板载 AXP2101，I2C）— 显示挪到摸头时呈现
- [x] 摸摸头（CST9217 触摸：被摸→弯眼笑+脸红+朝手指蹭+台词气泡+亮电量，松手回网络态）
- [x] 情绪逻辑 v2（信号/吞吐两轴 + 迟滞防抖不乱跳；载波/制式/上线事件冒短气泡；久闲打盹）
- [x] 翻页 + 数值页（轻点翻页查精确指标含版本号；手势与摸头区分：轻点翻页/长按摸头）
- [x] 电子吧唧·静态图（`tools/mkbadge.py` 烤 RGB565 → WiFi 拉 → 整屏 blit，设备零解码；45s 自动回脸防烧屏）
- [ ] 电子吧唧·GIF 动图页
- [ ] 路由器侧算 mood + MQTT 推送省电
- [ ] 深睡省电

## 美术

> 以下纯属玩梗，工程内容到上面为止 😌

**我是一个模拟太！**
对你是！台下 3k9x 欣喜若狂
**我有两个眼睛！！**
天呐下面 9x 狂欢呐
哦你好勇敢你讲出你是一个模拟太
