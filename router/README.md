# 路由器侧脚本（CPE / OpenWrt）

模拟太依赖路由器（NRadio CPE）上的两块东西。**这些不在固件里，路由器刷机/恢复出厂会丢**，故在此存档备份。

> 网关默认 `192.168.2.254`，root 登录。下面命令在路由器上跑。

## 1. `face_pub.sh` + `face_pub.init` — 数据出口

把蜂窝状态写成 `/www/face.json` 供 ESP32 轮询。uhttpd 默认就把 `/www` 当 web 根，所以 ESP32 直接 `GET http://<网关>/face.json`。

部署：
```sh
cp face_pub.sh        /usr/bin/face_pub.sh   && chmod +x /usr/bin/face_pub.sh
cp face_pub.init      /etc/init.d/face_pub   && chmod +x /etc/init.d/face_pub
/etc/init.d/face_pub enable && /etc/init.d/face_pub start
```
- 数据源：`ubus call infocd cpestatus`（MediaTek 模组）。字段见脚本 `get`。
- `online` 判断：蜂窝有 IP+mode **或** 有默认路由即在线（有线 WAN 模式也不误报「WAN 掉了」）。
- 吞吐读 `eth3`（模组口）；有线 WAN 模式下吞吐会是 0（少见工况，可接受）。

OTA 出口也放 `/www`：`monita.ver`(数字) + `monita-fw.bin` + `badge.m8g`（电子吧唧）。

## 2. `99-openclash-rewan` — OpenClash 跟随 WAN 切换

OpenClash 只在启动那刻绑定出口 WAN，**切 WAN（蜂窝↔有线）后不会重绑** → clash 拨节点全 `network unreachable`、上不了网。这个 hotplug 钩子在 WAN ifup/ifdown 时自动重启 OpenClash 让它重绑当前 WAN。

部署：
```sh
cp 99-openclash-rewan /etc/hotplug.d/iface/99-openclash-rewan && chmod +x /etc/hotplug.d/iface/99-openclash-rewan
```
- 防抖锁防止短时间多次触发狂重启。
- 若切 WAN 后仍不通：你的逻辑接口名可能不在 `case` 列表里（`wan|cpe|...`），加进去；临时手动 `/etc/init.d/openclash restart` 也能立刻恢复。
