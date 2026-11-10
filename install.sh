#!/bin/bash
# ./install.sh            编译并安装为开机自启的守护进程
# ./install.sh uninstall  卸载（卸载时会自动恢复充电）
set -euo pipefail
cd "$(dirname "$0")"

LABEL=com.local.chargelimit
PLIST=/Library/LaunchDaemons/$LABEL.plist
DAEMON=/Library/PrivilegedHelperTools/$LABEL     # 以 root 运行的守护进程：文件名与 Label 一致
CTL=/usr/local/bin/chargectl                      # 用户命令
CONF=/Library/Preferences/chargelimit.conf
OLD_FILES=(/usr/local/bin/chargelimitd /usr/local/etc/chargelimit.conf)   # 旧版本留下的文件
OLD_CONF=/usr/local/etc/chargelimit.conf

# 语言：LC_ALL / LC_MESSAGES / LANG 以 zh 开头用中文，否则用英文
case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in zh*) ZH=1 ;; *) ZH= ;; esac
msg() { if [[ $ZH ]]; then printf '%s\n' "$1"; else printf '%s\n' "$2"; fi; }
die() { msg "$1" "$2"; exit 1; }

stop() {   # SIGTERM → 守护进程恢复充电、清掉"运行中"标记；等服务真正卸载
  sudo launchctl bootout "system/$LABEL" 2>/dev/null || return 0
  for _ in $(seq 20); do
    sudo launchctl print "system/$LABEL" >/dev/null 2>&1 || return 0
    sleep 0.5
  done
  msg "警告：服务 10 秒内没有停止" "Warning: the service did not stop within 10 seconds"
}

safe_dir() {   # 以 root 运行的文件只能放在：路径上每一级都归 root 所有、对他人不可写
  local d=$1 uid mode
  while :; do
    read -r uid mode < <(stat -f '%u %Lp' "$d") \
      || die "无法读取目录属性：$d，停止安装" "Cannot stat $d; aborting"
    if [[ $uid != 0 ]] || (( 8#$mode & 8#022 )); then
      die "不安全的目录：$d（属主 uid $uid，权限 $mode），停止安装" \
          "Unsafe directory: $d (owner uid $uid, mode $mode); aborting"
    fi
    [[ $d == / ]] && return 0
    d=$(dirname "$d")
  done
}

if [[ "${1:-}" == "uninstall" ]]; then
  stop
  if [[ -x $DAEMON ]]; then sudo "$DAEMON" allow || true; fi   # 保险：守护进程没在运行时也恢复充电
  sudo rm -f "$PLIST" "$DAEMON" "$CTL" /var/db/chargelimit.state "${OLD_FILES[@]}"
  msg "已卸载（配置文件 $CONF 保留）" "Uninstalled (config $CONF kept)"
  exit 0
fi

clang -O2 -Wall -Wextra -o chargelimitd chargelimitd.c -framework IOKit -framework CoreFoundation

echo "== SMC =="   # 用 root 检测：这就是马上要以 root 运行的程序
rc=0; sudo ./chargelimitd status || rc=$?
case $rc in
  0) ;;
  1) die "未找到可用的充电控制键，停止安装" "No supported charging key found; aborting" ;;
  *) die "无法打开 AppleSMC，停止安装" "Cannot open AppleSMC; aborting" ;;
esac

for d in "$(dirname "$DAEMON")" "$(dirname "$CTL")" "$(dirname "$CONF")" "$(dirname "$PLIST")"; do
  [[ -d $d ]] || sudo install -d -m 755 -o root -g wheel "$d"   # 只创建缺失的目录，不改动已有目录
  safe_dir "$d"
done

stop
sudo install -m 755 -o root -g wheel chargelimitd   "$DAEMON"
sudo install -m 755 -o root -g wheel chargectl.bash "$CTL"

if [[ ! -f $CONF ]]; then
  tmp=$(mktemp)
  if [[ -f $OLD_CONF ]]; then cp "$OLD_CONF" "$tmp"          # 迁移旧配置（缺的项用默认值）
  else printf "enabled=1\nupper=80\nlower=60\ncalib_every=3\ncalib_days=30\ndischarge=1\n" > "$tmp"; fi
  sudo install -m 644 -o root -g wheel "$tmp" "$CONF"
  rm -f "$tmp"
  if [[ -f $OLD_CONF ]]; then   # 旧目录可能曾被他人改写，内容要人工看一眼
    msg "已从 $OLD_CONF 迁移配置，请确认：" "Migrated config from $OLD_CONF; please check:"
    sed 's/^/    /' "$CONF"
  fi
fi
sudo rm -f "${OLD_FILES[@]}"

sudo install -m 644 -o root -g wheel $LABEL.plist "$PLIST"
sudo launchctl bootstrap system "$PLIST"
sleep 2   # 等守护进程启动、写好状态

echo "== OK =="
"$CTL" status || true   # 状态已打印；非 0 也不要中断，后面的提醒必须显示
echo
msg "提醒：如果上限高于 80%，或希望校准按时完成，请关闭 系统设置 → 电池 → 电池健康 ⓘ →「优化电池充电」。" \
    "Note: if the upper limit is above 80%, or you want calibration on schedule, turn off
System Settings → Battery → Battery Health ⓘ → Optimized Battery Charging."
