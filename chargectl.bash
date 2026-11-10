#!/bin/bash
# chargectl — 设置充电上下限（install.sh 安装为 /usr/local/bin/chargectl）
# status 的退出码：0 正常，1 没有可用的充电控制键，2 打不开 AppleSMC（状态信息照常打印）
set -euo pipefail

BIN=/Library/PrivilegedHelperTools/com.local.chargelimit
CONF=/Library/Preferences/chargelimit.conf
LABEL=com.local.chargelimit
PLIST=/Library/LaunchDaemons/$LABEL.plist

# 语言：LC_ALL / LC_MESSAGES / LANG 以 zh 开头用中文，否则用英文
case "${LC_ALL:-${LC_MESSAGES:-${LANG:-}}}" in zh*) ZH=1 ;; *) ZH= ;; esac
msg() { if [[ $ZH ]]; then printf '%s\n' "$1"; else printf '%s\n' "$2"; fi; }
die() { msg "$1" "$2"; exit 1; }

usage() {
  if [[ $ZH ]]; then cat <<'EOF'
  chargectl status              查看状态
  sudo chargectl set 80 60      上限 80%，下限 60%（同时启用限制）
  sudo chargectl calib 3 30     每触发上限 3 次或每 30 天校准一次（充到 100% 保持 5 分钟）；0 = 关闭该条件
  sudo chargectl off            关闭限制（恢复正常充满）
  sudo chargectl on             启用限制；如果之前因"停充无效"被停用，清除后重新验证
  chargectl log                 查看最近 2 小时的日志
EOF
  else cat <<'EOF'
  chargectl status              show status
  sudo chargectl set 80 60      upper 80%, lower 60% (also turns the limiter on)
  sudo chargectl calib 3 30     calibrate (charge to 100% and hold 5 min) every 3 limits or 30 days; 0 = off
  sudo chargectl off            turn the limiter off (charge to full as usual)
  sudo chargectl on             turn the limiter on; clears a "had no effect" disable and re-verifies
  chargectl log                 show the last 2 hours of logs
EOF
  fi
}

admin() {   # 修改配置前：需要 root，且守护进程已安装
  [[ $EUID -eq 0 ]] || die "请用 sudo 运行" "Please run with sudo"
  [[ -x $BIN ]]     || die "守护进程未安装，请先运行 install.sh" "The daemon is not installed; run install.sh first"
}
is_int() { [[ $1 =~ ^[0-9]+$ ]] || die "必须是整数: $1" "Not an integer: $1"; }

get() {   # get <键> <默认值>：只接受整数，其他一律用默认值
  local v
  v=$(grep -E "^$1=" "$CONF" 2>/dev/null | tail -1 | cut -d= -f2- | tr -d '[:space:]') || true
  if [[ $v =~ ^[0-9]+$ ]]; then echo "$v"; else echo "$2"; fi
}

save() {   # save <enabled> <upper> <lower> <calib_every> <calib_days>
  local dir uid mode tmp
  dir=$(dirname "$CONF")
  read -r uid mode < <(stat -f '%u %Lp' "$dir") || die "无法读取目录属性：$dir" "Cannot stat $dir"
  if [[ $uid != 0 ]] || (( 8#$mode & 8#022 )); then
    die "拒绝写入：$dir 不是 root 所有或对他人可写" "Refusing to write: $dir is not root-owned or is writable by others"
  fi
  tmp=$(mktemp "$CONF.XXXXXX") || die "无法创建临时文件" "Cannot create a temporary file"   # 随机名 + O_EXCL
  if ! printf "enabled=%d\nupper=%d\nlower=%d\ncalib_every=%d\ncalib_days=%d\n" "$@" > "$tmp" \
     || ! chmod 644 "$tmp" || ! mv -f "$tmp" "$CONF"; then                 # rename 不跟随链接
    rm -f "$tmp"; die "写入配置失败" "Failed to write the config"
  fi
  # 守护进程在运行 → HUP 立即生效（它也每 30 秒重读配置）；进程已退出但服务已加载 → 启动它
  launchctl kill HUP "system/$LABEL" 2>/dev/null \
    || launchctl kickstart "system/$LABEL" 2>/dev/null \
    || msg "（守护进程未加载，配置已保存。加载：sudo launchctl bootstrap system $PLIST）" \
           "(The daemon is not loaded; config saved. Load it: sudo launchctl bootstrap system $PLIST)"
}

en=$(get enabled 1); up=$(get upper 80); lo=$(get lower 60)
ce=$(get calib_every 3); cdays=$(get calib_days 30)

case "${1:-status}" in
  status)
    "$BIN" status ;;
  set)
    admin
    [[ $# -ge 3 ]] || die "用法: chargectl set <上限> <下限>" "Usage: chargectl set <upper> <lower>"
    up=$2; lo=$3
    is_int "$up"; is_int "$lo"
    (( up >= 20 && up <= 100 )) || die "上限需在 20–100" "Upper must be 20–100"
    (( lo >= 5 && lo <= up - 5 )) || die "下限需 >= 5 且比上限至少低 5" "Lower must be >= 5 and at least 5 below upper"
    save 1 "$up" "$lo" "$ce" "$cdays"
    msg "已设置：上限 ${up}%，下限 ${lo}%" "Set: upper ${up}%, lower ${lo}%" ;;
  calib)
    admin
    [[ $# -ge 2 ]] || die "用法: chargectl calib <次数> [天数]" "Usage: chargectl calib <limits> [days]"
    ce=$2; cdays=${3:-$cdays}
    is_int "$ce"; is_int "$cdays"
    (( ce <= 50 ))     || die "次数需在 0–50" "Limits must be 0–50"
    (( cdays <= 365 )) || die "天数需在 0–365" "Days must be 0–365"
    save "$en" "$up" "$lo" "$ce" "$cdays"
    msg "已设置：校准条件 次数 ${ce}（0 = 不按次数），天数 ${cdays}（0 = 不按天数）" \
        "Set: calibrate every ${ce} limits (0 = off) or ${cdays} days (0 = off)" ;;
  on)
    admin
    "$BIN" clearfault
    save 1 "$up" "$lo" "$ce" "$cdays"
    msg "限制已启用" "Limiter on" ;;
  off)
    admin
    save 0 "$up" "$lo" "$ce" "$cdays"
    msg "限制已关闭" "Limiter off" ;;
  log)   # 安装后的进程名是 com.local.chargelimit；开发时直接运行编译产物则是 chargelimitd
    log show --last 2h --style compact \
      --predicate 'process == "com.local.chargelimit" OR process == "chargelimitd"' ;;
  *)
    usage ;;
esac
