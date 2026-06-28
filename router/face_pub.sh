#!/bin/sh
# 小圆脸数据发布器：周期性把 infocd 蜂窝状态精简成 /www/face.json，供 ESP32 轮询。
# 原子写（先写 .tmp 再 mv），避免 ESP32 读到半截。吞吐在采样间隔内求平均。
# 部署：放到路由器 /usr/bin/face_pub.sh，配 /etc/init.d/face_pub 服务（见 face_pub.init）。
OUT=/www/face.json
TMP=/www/.face.json.tmp
IF=eth3            # WAN(模组)口
INTERVAL=3        # 秒；同时作为吞吐采样窗口
PIDF=/tmp/face_pub.pid

# 杀掉残留实例
for p in $(ps w 2>/dev/null | grep 'face_pub\.sh' | grep -v grep | grep -v "$$" | awk '{print $1}'); do
  kill -9 "$p" 2>/dev/null
done
echo $$ > "$PIDF"

num(){ [ -n "$1" ] && printf '%s' "$1" || printf 'null'; }
get(){ printf '%s' "$J" | jsonfilter -e "@.result[0].status.$1" 2>/dev/null; }

while true; do
  R1=$(cat /sys/class/net/$IF/statistics/rx_bytes 2>/dev/null)
  T1=$(cat /sys/class/net/$IF/statistics/tx_bytes 2>/dev/null)
  sleep "$INTERVAL"
  R2=$(cat /sys/class/net/$IF/statistics/rx_bytes 2>/dev/null)
  T2=$(cat /sys/class/net/$IF/statistics/tx_bytes 2>/dev/null)
  DL=$(( (${R2:-0} - ${R1:-0}) / INTERVAL ))
  UL=$(( (${T2:-0} - ${T1:-0}) / INTERVAL ))

  J=$(ubus call infocd cpestatus '{}' 2>/dev/null)
  RSRP=$(get rsrp); RSRQ=$(get rsrq); SINR=$(get sinr); CQI=$(get CQI)
  MODE=$(get mode); BAND=$(get band); BAND1=$(get band1); BC=$(get band_count)
  TEMP=$(get model_temp); CPU=$(get cpu_percent); MEM=$(get mem_percent)
  ISP=$(get isp); SIM=$(get simno)
  IP=$(printf '%s' "$J" | jsonfilter -e '@.result[0].status.V4.ipaddrs[0]' 2>/dev/null)
  ONLINE=0; [ -n "$IP" ] && [ -n "$MODE" ] && ONLINE=1
  ip -4 route show default 2>/dev/null | grep -q . && ONLINE=1   # 有线/蜂窝任一有默认路由即在线（不误报 WAN 掉了）

  # 设备列表（DHCP 租约）：在线设备数 + 检测新上线主机名（供「是新朋友！」反应）
  MACS=$(awk '{print $2}' /tmp/dhcp.leases 2>/dev/null | sort -u)
  CLIENTS=$(printf '%s\n' "$MACS" | grep -c .); CLIENTS=${CLIENTS:-0}
  NEWMAC=""
  if [ -s /tmp/face_devs ]; then
    for m in $MACS; do grep -qx "$m" /tmp/face_devs 2>/dev/null || { NEWMAC="$m"; break; }; done
  fi
  printf '%s\n' "$MACS" > /tmp/face_devs
  NEWDEV=""
  [ -n "$NEWMAC" ] && NEWDEV=$(awk -v m="$NEWMAC" 'tolower($2)==tolower(m){print $4; exit}' /tmp/dhcp.leases)
  [ "$NEWDEV" = "*" ] && NEWDEV=""

  TS=$(date +%s)

  printf '{"ts":%s,"online":%s,"mode":"%s","rsrp":%s,"rsrq":%s,"sinr":%s,"cqi":%s,"band_count":%s,"band":"%s","band1":"%s","temp":%s,"cpu":%s,"mem":%s,"dl_bps":%s,"ul_bps":%s,"isp":"%s","sim":"%s","clients":%s,"newdev":"%s"}\n' \
    "$TS" "$ONLINE" "$MODE" "$(num "$RSRP")" "$(num "$RSRQ")" "$(num "$SINR")" "$(num "$CQI")" \
    "$(num "$BC")" "$BAND" "$BAND1" "$(num "$TEMP")" "$(num "$CPU")" "$(num "$MEM")" \
    "$DL" "$UL" "$ISP" "$SIM" "$CLIENTS" "$NEWDEV" > "$TMP" 2>/dev/null
  mv "$TMP" "$OUT" 2>/dev/null
done
