// chargelimitd.c — Apple Silicon MacBook 充电上下限守护进程
//
// 逻辑（滞回）：电量 >= upper → 停止充电；<= lower → 恢复充电；介于两者之间保持当前状态。
// 停止充电时，接着电源的 Mac 直接由适配器供电，电池电量基本不变。
// 插电放电：接着电源且电量高于上限时，断开适配器供电让电池放电，降到上限以下 1% 再恢复。
// 校准：触发上限 calib_every 次后的下一次，或距上次校准满 calib_days 天，充到 100% 保持 5 分钟。
//
// SMC 键（未公开，参考 AlDente / battery 等开源工具）：
//   充电  CHTE        较新的固件，4 字节：01000000 停止充电，00000000 允许充电
//         CH0B/CH0C   旧固件，1 字节：02 停止充电，00 允许充电
//   适配器 CHIE       较新的固件，1 字节：08 断开适配器供电，00 恢复
//         CH0I        旧固件，1 字节：01 断开，00 恢复
//
// 安全：
//   - 每次写入都回读确认；第一次停充后 60 秒确认电池真的不再充电。
//   - 停充无效时恢复充电并停用；在同一个系统 + 固件版本上不再写，直到 `chargelimitd clearfault`。
//   - 适配器：任何退出（信号、出错、崩溃后重启、开机）和睡眠前都先恢复供电；放电不会低于上限 - 1。
//     第一次断开后 60 秒确认系统真的改用电池；无效时只停用放电，停充照常。
//   - 配置文件必须归 root 所有且他人不可写，否则忽略并使用默认值。
//
// 编译：clang -O2 -Wall -Wextra -o chargelimitd chargelimitd.c -framework IOKit -framework CoreFoundation
// 安装：install.sh 把它装成 /Library/PrivilegedHelperTools/com.local.chargelimit（与 launchd Label 同名）
// 用法：chargelimitd status | probe | sudo chargelimitd run | inhibit | allow | clearfault | adaptertest [秒]
//       status 退出码：0 正常，1 没有可用的充电控制键，2 打不开 AppleSMC
//       probe：只读，列出控制键；adaptertest：断开适配器 N 秒（默认 60）后一定恢复，0 = 只恢复
//       守护进程信号：TERM/INT 恢复后退出，HUP 重读配置，USR1 清除放电的停用状态

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <syslog.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <libproc.h>
#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include "policy.h"

// 面向用户的输出：LC_ALL / LC_MESSAGES / LANG 以 zh 开头用中文，否则用英文。
// 日志和状态文件里的记录始终是英文。
static bool g_zh;
#define L(zh, en) (g_zh ? (zh) : (en))

static void lang_init(void) {
    const char *v = getenv("LC_ALL");
    if (!v || !*v) v = getenv("LC_MESSAGES");
    if (!v || !*v) v = getenv("LANG");
    g_zh = v && !strncmp(v, "zh", 2);
}

#define CONF_PATH   "/Library/Preferences/chargelimit.conf"
#define STATE_PATH  "/var/db/chargelimit.state"
#define TICK_SEC    30
#define VERIFY_SEC  60     // 停充后过这么久还在充电 → 键没有生效
#define STALL_SEC   600    // 允许充电却持续不充电这么久 → 记日志、在 status 里提示
#define FIGHT_N     10     // FIGHT_SEC 内键被外部改写这么多次 → 判定有其他程序争抢，退出
#define FIGHT_SEC   600
#define IO_FAIL_MAX 3      // 连续读写失败这么多次 → 退出（断开适配器失败则停用放电）
#define DPAUSE_SEC  600    // 断开适配器失败后，过这么久再重试
#define LOG_PATH    "/var/db/chargelimit.log"
#define LOG_MAX     (256 * 1024)

// ---------------- SMC ----------------

typedef struct { char major, minor, build, reserved; uint16_t release; } SMCVers;
typedef struct { uint16_t version, length; uint32_t cpuPLimit, gpuPLimit, memPLimit; } SMCPLimit;
typedef struct { uint32_t dataSize; uint32_t dataType; uint8_t dataAttributes; } SMCKeyInfo;
typedef struct {
    uint32_t   key;
    SMCVers    vers;
    SMCPLimit  pLimit;
    SMCKeyInfo keyInfo;
    uint8_t    result, status, data8;
    uint32_t   data32;
    uint8_t    bytes[32];
} SMCParam;
_Static_assert(sizeof(SMCParam) == 80, "SMCParam layout mismatch");

enum { SMC_SELECTOR = 2, SMC_CMD_READ = 5, SMC_CMD_WRITE = 6, SMC_CMD_KEYINFO = 9 };

static io_connect_t g_smc;

static bool smc_open(void) {
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    if (!s) return false;
    kern_return_t kr = IOServiceOpen(s, mach_task_self(), 0, &g_smc);
    IOObjectRelease(s);
    return kr == KERN_SUCCESS;
}

static uint32_t k4(const char *k) {
    return (uint32_t)k[0] << 24 | (uint32_t)k[1] << 16 | (uint32_t)k[2] << 8 | (uint32_t)k[3];
}

static bool smc_call(SMCParam *in, SMCParam *out) {
    size_t sz = sizeof(SMCParam);
    return IOConnectCallStructMethod(g_smc, SMC_SELECTOR, in, sizeof(SMCParam), out, &sz) == KERN_SUCCESS
        && out->result == 0;
}

static uint32_t smc_size(const char *key) {     // 0 = 键不存在
    SMCParam in = {0}, out = {0};
    in.key = k4(key);
    in.data8 = SMC_CMD_KEYINFO;
    if (!smc_call(&in, &out) || out.keyInfo.dataSize > 32) return 0;
    return out.keyInfo.dataSize;
}

static bool smc_read(const char *key, uint32_t size, uint8_t *buf) {
    SMCParam in = {0}, out = {0};
    in.key = k4(key);
    in.keyInfo.dataSize = size;
    in.data8 = SMC_CMD_READ;
    if (!smc_call(&in, &out)) return false;
    memcpy(buf, out.bytes, size);
    return true;
}

static bool smc_write(const char *key, uint32_t size, uint8_t v) {   // byte0 = v，其余补 0
    SMCParam in = {0}, out = {0};
    in.key = k4(key);
    in.keyInfo.dataSize = size;
    in.data8 = SMC_CMD_WRITE;
    in.bytes[0] = v;
    return smc_call(&in, &out);
}

// ---------------- 控制键：充电、适配器 ----------------
// on = 停止充电 / 断开适配器供电；off = 正常状态

typedef struct { const char *name; const char *keys[2]; uint32_t size; uint8_t on, off; } Ctrl;
static const Ctrl CHARGE_KEYS[] = {
    { "CHTE",      { "CHTE", NULL   }, 4, 1, 0 },
    { "CH0B/CH0C", { "CH0B", "CH0C" }, 1, 2, 0 },
    { NULL, { NULL, NULL }, 0, 0, 0 },
};
static const Ctrl ADAPTER_KEYS[] = {
    { "CHIE", { "CHIE", NULL }, 1, 0x08, 0 },
    { "CH0I", { "CH0I", NULL }, 1, 0x01, 0 },
    { NULL, { NULL, NULL }, 0, 0, 0 },
};
static const Ctrl *g_ctrl;       // 充电控制键
static const Ctrl *g_adapter;    // 适配器控制键；NULL = 不支持插电放电
static char g_ctrl_diag[96];     // 充电控制键检测失败时说明原因

// 键必须存在且长度符合预期；长度不对说明固件改了定义，不能盲写
static const Ctrl *ctrl_detect(const Ctrl *t, char *diag, size_t n) {
    for (; t->name; t++) {
        bool ok = true;
        for (int j = 0; j < 2 && t->keys[j]; j++) {
            uint32_t sz = smc_size(t->keys[j]);
            if (sz != t->size) {
                if (sz && diag && !diag[0])
                    snprintf(diag, n, L("%s 存在但长度为 %u（预期 %u）", "%s exists but has size %u (expected %u)"),
                             t->keys[j], sz, t->size);
                ok = false;
            }
        }
        if (ok) return t;
    }
    return NULL;
}

static int ctrl_read(const Ctrl *c) {   // 1 = on，0 = off，-1 = 读取失败或出现意外的值
    int r = -2;
    for (int j = 0; j < 2 && c->keys[j]; j++) {
        uint8_t b[32];
        if (!smc_read(c->keys[j], c->size, b)) return -1;
        int v = b[0] == c->on ? 1 : b[0] == c->off ? 0 : -1;
        if (v < 0 || (r != -2 && v != r)) return -1;
        r = v;
    }
    return r;
}

static bool ctrl_write(const Ctrl *c, bool on) {   // 写入后回读确认
    for (int j = 0; j < 2 && c->keys[j]; j++)
        if (!smc_write(c->keys[j], c->size, on ? c->on : c->off)) return false;
    return ctrl_read(c) == (on ? 1 : 0);
}

// 第一个键的原始值（十六进制），用于诊断写入失败
static void ctrl_raw(const Ctrl *c, char *buf, size_t n) {
    uint8_t b[32];
    snprintf(buf, n, "?");
    if (!smc_read(c->keys[0], c->size, b)) return;
    for (uint32_t i = 0; i < c->size && 2 * i + 2 < n; i++) snprintf(buf + 2 * i, n - 2 * i, "%02x", b[i]);
}

static bool g_adapter_off;        // 当前已断开适配器供电（正在放电）

static int  key_read(void)        { return ctrl_read(g_ctrl); }
static bool key_write(bool on)    { return ctrl_write(g_ctrl, on); }
static bool adapter_restore(void) { return !g_adapter || ctrl_write(g_adapter, false); }   // 恢复适配器供电

// 恢复正常：先恢复适配器供电（最重要），再允许充电；两个都要执行，不能短路
static bool restore_all(void) {
    bool ok = adapter_restore();
    ok = (g_ctrl && key_write(false)) && ok;
    g_adapter_off = false;
    return ok;
}

// ---------------- 电池 ----------------

typedef struct { int pct, ma; bool ac, charging, ok; } Batt;   // ma：电池电流，负数 = 放电

static Batt batt_read(void) {
    Batt b = {0};
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (!info) return b;
    CFArrayRef list = IOPSCopyPowerSourcesList(info);
    for (CFIndex i = 0; list && i < CFArrayGetCount(list); i++) {
        CFDictionaryRef d = IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(list, i));
        if (!d) continue;
        CFStringRef type = CFDictionaryGetValue(d, CFSTR(kIOPSTypeKey));
        if (!type || !CFEqual(type, CFSTR(kIOPSInternalBatteryType))) continue;

        int cur = 0, max = 0;
        CFNumberRef n;
        if ((n = CFDictionaryGetValue(d, CFSTR(kIOPSCurrentCapacityKey)))) CFNumberGetValue(n, kCFNumberIntType, &cur);
        if ((n = CFDictionaryGetValue(d, CFSTR(kIOPSMaxCapacityKey))))     CFNumberGetValue(n, kCFNumberIntType, &max);
        if ((n = CFDictionaryGetValue(d, CFSTR(kIOPSCurrentKey))))         CFNumberGetValue(n, kCFNumberIntType, &b.ma);
        CFStringRef st = CFDictionaryGetValue(d, CFSTR(kIOPSPowerSourceStateKey));
        b.ac = st && CFEqual(st, CFSTR(kIOPSACPowerValue));
        CFBooleanRef ch = CFDictionaryGetValue(d, CFSTR(kIOPSIsChargingKey));
        b.charging = ch && CFBooleanGetValue(ch);
        if (max > 0) { b.pct = (cur * 100 + max / 2) / max; b.ok = true; }
        break;
    }
    if (list) CFRelease(list);
    CFRelease(info);
    return b;
}

// ---------------- 系统信息 ----------------

static long boot_time(void) {
    struct timeval tv = {0};
    size_t len = sizeof tv;
    return sysctlbyname("kern.boottime", &tv, &len, NULL, 0) == 0 ? (long)tv.tv_sec : 0;
}

static void sysctl_str(const char *name, char *buf, size_t n) {
    size_t len = n;
    if (sysctlbyname(name, buf, &len, NULL, 0) != 0) snprintf(buf, n, "?");
}

// 系统固件版本（与 system_profiler 的 System Firmware Version 相同），读不到时为 "?"
static void fw_version(char *buf, size_t n) {
    snprintf(buf, n, "?");
    io_registry_entry_t e = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/chosen");
    if (!e) return;
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, CFSTR("system-firmware-version"), kCFAllocatorDefault, 0);
    IOObjectRelease(e);
    if (!v) return;
    if (CFGetTypeID(v) == CFDataGetTypeID()) {
        CFIndex len = CFDataGetLength((CFDataRef)v);
        if (len > (CFIndex)n - 1) len = (CFIndex)n - 1;
        memcpy(buf, CFDataGetBytePtr((CFDataRef)v), (size_t)len);
        buf[len] = 0;
    } else if (CFGetTypeID(v) == CFStringGetTypeID()) {
        CFStringGetCString((CFStringRef)v, buf, (CFIndex)n, kCFStringEncodingUTF8);
    }
    CFRelease(v);
    if (!strncmp(buf, "iBoot-", 6)) memmove(buf, buf + 6, strlen(buf + 6) + 1);
}

typedef struct { char os[32], fw[48]; } Build;   // 系统版本（kern.osversion）+ 固件版本

static Build build_now(void) {
    Build b;
    sysctl_str("kern.osversion", b.os, sizeof b.os);
    fw_version(b.fw, sizeof b.fw);
    return b;
}

static void stamp(time_t t, char *buf, size_t n) { strftime(buf, n, "%Y-%m-%d %H:%M:%S", localtime(&t)); }

// ---------------- 配置 ----------------

static bool g_conf_ignored;      // 配置文件属主或权限不安全，已忽略

static Conf conf_read(void) {
    Conf c = { .enabled = true, .upper = 80, .lower = 60, .calib_every = 3, .calib_days = 30, .discharge = true };
    g_conf_ignored = false;
    int fd = open(CONF_PATH, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return conf_sanitize(c);
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022)) {
        g_conf_ignored = true;
        close(fd);
        return conf_sanitize(c);
    }
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return conf_sanitize(c); }
    char line[128]; int v;
    while (fgets(line, sizeof line, f)) {
        if      (sscanf(line, "upper=%d", &v) == 1)       c.upper = v;
        else if (sscanf(line, "lower=%d", &v) == 1)       c.lower = v;
        else if (sscanf(line, "enabled=%d", &v) == 1)     c.enabled = (v != 0);
        else if (sscanf(line, "calib_every=%d", &v) == 1) c.calib_every = v;
        else if (sscanf(line, "calib_days=%d", &v) == 1)  c.calib_days = v;
        else if (sscanf(line, "discharge=%d", &v) == 1)   c.discharge = (v != 0);
    }
    fclose(f);
    return conf_sanitize(c);
}

// ---------------- 日志 ----------------
// 同时写系统日志和 /var/db/chargelimit.log（chargectl log 读这个文件）。超过 LOG_MAX 时把旧文件改名为 .1。
// 只有 root 能写；普通用户运行时只写系统日志。

static void logm(int pri, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logm(int pri, const char *fmt, ...) {
    char msg[256], ts[24];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    syslog(pri, "%s", msg);

    struct stat st;
    if (stat(LOG_PATH, &st) == 0 && st.st_size > LOG_MAX) rename(LOG_PATH, LOG_PATH ".1");
    int fd = open(LOG_PATH, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW, 0644);
    if (fd < 0) return;
    stamp(time(NULL), ts, sizeof ts);
    dprintf(fd, "%s %s%s\n", ts, pri <= LOG_ERR ? "ERROR " : pri == LOG_WARNING ? "WARN " : "", msg);
    close(fd);
}

// ---------------- 状态文件 ----------------

typedef struct {
    Policy p;
    int    running;       // 1 = 守护进程在运行；干净退出时清零
    int    pid;
    long   boot;          // 写入时的开机时间：开机时间不同说明那个 pid 早已不存在
    char   os[32];        // 最近一次验证键时的系统版本（kern.osversion）
    char   fw[48];        // 最近一次验证键时的固件版本
    char   ctrl[16];      // 最近一次验证时用的键
    long   verified;      // 最近一次写入并回读成功的时间
    int    functional;    // 1 = 已确认停充生效，0 = 尚未确认，-1 = 停充无效（停用）
    int    dfunctional;   // 插电放电：1 = 已确认断开适配器生效，0 = 尚未确认，-1 = 无效（只停用放电）
    long   dpause;        // 插电放电暂停：0 = 没有，> 0 = 暂停到这个时间，-1 = 连续失败已停用（本次运行）
    char   dfail[128];    // 最近一次断开适配器失败的情况（含读回的原始值）
    int    overrides;     // 键被外部改写的累计次数
    long   stall_since;   // "允许充电却不充"开始的时间；0 = 没有
    char   override[160]; // 最近一次外部改写的情况
    char   last[192];     // 最近一次动作及原因
} State;

static State g_st;
static bool  g_noted;     // note() 改写了"最近动作"，需要保存

static void state_load(State *s) {
    memset(s, 0, sizeof *s);
    FILE *f = fopen(STATE_PATH, "r");
    if (!f) return;
    char line[256], k[32], v[224];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%31[^=]=%223[^\n]", k, v) != 2) continue;
        long n = strtol(v, NULL, 10);
        if      (!strcmp(k, "inhibited"))   s->p.inhibited = n;
        else if (!strcmp(k, "calibrating")) s->p.calibrating = n;
        else if (!strcmp(k, "triggers"))    s->p.triggers = (int)n;
        else if (!strcmp(k, "last_calib"))  s->p.last_calib = n;
        else if (!strcmp(k, "calib_since")) s->p.calib_since = n;
        else if (!strcmp(k, "running"))     s->running = (int)n;
        else if (!strcmp(k, "pid"))         s->pid = (int)n;
        else if (!strcmp(k, "boot"))        s->boot = n;
        else if (!strcmp(k, "os"))          snprintf(s->os, sizeof s->os, "%s", v);
        else if (!strcmp(k, "fw"))          snprintf(s->fw, sizeof s->fw, "%s", v);
        else if (!strcmp(k, "ctrl"))        snprintf(s->ctrl, sizeof s->ctrl, "%s", v);
        else if (!strcmp(k, "verified"))    s->verified = n;
        else if (!strcmp(k, "functional"))  s->functional = (int)n;
        else if (!strcmp(k, "dfunctional")) s->dfunctional = (int)n;
        else if (!strcmp(k, "dpause"))      s->dpause = n;
        else if (!strcmp(k, "dfail"))       snprintf(s->dfail, sizeof s->dfail, "%s", v);
        else if (!strcmp(k, "overrides"))   s->overrides = (int)n;
        else if (!strcmp(k, "stall_since")) s->stall_since = n;
        else if (!strcmp(k, "override"))    snprintf(s->override, sizeof s->override, "%s", v);
        else if (!strcmp(k, "last"))        snprintf(s->last, sizeof s->last, "%s", v);
    }
    fclose(f);
}

// 写临时文件再改名，读者永远看到完整内容；O_EXCL|O_NOFOLLOW 防止被预置的链接利用
static void state_save(const State *s) {
    static const char tmp[] = STATE_PATH ".tmp";
    unlink(tmp);                                       // 上次中断留下的临时文件
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) {
        logm(LOG_ERR, "cannot write %s: %s", tmp, strerror(errno));
        if (fd >= 0) close(fd);
        return;
    }
    fchmod(fd, 0644);
    fprintf(f, "running=%d\npid=%d\nboot=%ld\ninhibited=%d\ncalibrating=%d\ntriggers=%d\n"
               "last_calib=%ld\ncalib_since=%ld\nos=%s\nfw=%s\nctrl=%s\nverified=%ld\n"
               "functional=%d\ndfunctional=%d\ndpause=%ld\ndfail=%s\n"
               "overrides=%d\nstall_since=%ld\noverride=%s\nlast=%s\n",
            s->running, s->pid, s->boot, s->p.inhibited, s->p.calibrating, s->p.triggers,
            (long)s->p.last_calib, (long)s->p.calib_since, s->os, s->fw, s->ctrl, s->verified,
            s->functional, s->dfunctional, s->dpause, s->dfail,
            s->overrides, s->stall_since, s->override, s->last);
    bool ok = !ferror(f) && fflush(f) == 0 && fsync(fd) == 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok)
        logm(LOG_ERR, "cannot write %s: %s", tmp, strerror(errno));
    else if (rename(tmp, STATE_PATH) != 0)
        logm(LOG_ERR, "cannot rename %s to %s: %s", tmp, STATE_PATH, strerror(errno));
    else
        return;
    unlink(tmp);
}

static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...) {   // 记日志，同时作为状态文件里的"最近动作"
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_st.last, sizeof g_st.last, fmt, ap);
    va_end(ap);
    g_noted = true;
    logm(LOG_NOTICE, "%s", g_st.last);
}

// 记录的 pid 是否仍是我们的守护进程。进程名会被截断到 16 个字符，所以比较可执行文件的完整路径：
// 安装后叫 com.local.chargelimit，开发时直接运行编译产物叫 chargelimitd。
static bool daemon_alive(const State *s) {
    if (!s->running || s->pid <= 0 || s->boot != boot_time()) return false;
    if (kill(s->pid, 0) != 0 && errno != EPERM) return false;
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(s->pid, path, sizeof path) <= 0) return true;   // 读不到路径：按仍在运行处理
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    return !strcmp(base, "com.local.chargelimit") || !strcmp(base, "chargelimitd");
}

// 守护进程上次没有正常退出（崩溃、被 SIGKILL、断电）：先恢复适配器供电和充电，再执行这次的命令
static void recover_if_crashed(State *s) {
    if (!s->running || daemon_alive(s)) return;
    bool ok = restore_all();
    s->running = 0;
    note("previous daemon (pid %d) exited abnormally; adapter and charging reset%s", s->pid, ok ? "" : " FAILED");
    state_save(s);
}

static bool same_build(void) {   // 当前系统 + 固件版本与上次验证时相同
    Build b = build_now();
    return !strcmp(b.os, g_st.os) && !strcmp(b.fw, g_st.fw);
}

// 方案 B：本系统 + 固件版本上已确认停充无效 → 不再写键
static bool fault_blocks(void) { return g_st.functional == -1 && same_build(); }

// 记录本次验证：系统、固件版本或键变了，之前的功能验证结论作废（停充和放电都是）
static void record_verified(void) {
    Build b = build_now();
    if (!same_build() || strcmp(g_ctrl->name, g_st.ctrl)) g_st.functional = g_st.dfunctional = 0;
    snprintf(g_st.os, sizeof g_st.os, "%s", b.os);
    snprintf(g_st.fw, sizeof g_st.fw, "%s", b.fw);
    snprintf(g_st.ctrl, sizeof g_st.ctrl, "%s", g_ctrl->name);
    g_st.verified = time(NULL);
}

// ---------------- 守护进程 ----------------

static int    g_written = -1;     // 本进程最后写入的值：1 停充，0 允许，-1 未知
static int    g_prev_pct = -1, g_prev_ac = -1, g_io_fail, g_adapter_fail;
static time_t g_full_since, g_check_at, g_dcheck_at, g_ac_change, g_wake_at;
static bool   g_stall_logged;
static int    g_dfail;            // 连续断开适配器失败的次数
static time_t g_fights[FIGHT_N];  // 最近几次外部改写的时间（环形）
static int    g_fight_i;

// 恢复正常并退出（exit 0，launchd 不会立即重启）。用户停止（信号）和出错（fatal）都走这里。
static void finish(const char *why) __attribute__((noreturn));
static void finish(const char *why) {
    bool ok = restore_all();
    note("stopped: %s; adapter and charging re-enabled%s", why, ok ? "" : " FAILED");
    g_st.running = 0;
    state_save(&g_st);
    exit(0);
}

static void fatal(const char *why) __attribute__((noreturn));
static void fatal(const char *why) { logm(LOG_ERR, "%s", why); finish(why); }

static bool apply(bool on, const Batt *b, const char *why) {
    if (!key_write(on)) {
        g_written = -1;                  // 写了一半也有可能，值未知
        logm(LOG_ERR, "SMC write/readback failed (%s)", why);
        return false;
    }
    g_written = on;
    note("%s charging: %s", on ? "stop" : "resume", why);
    if (on && b->ac && b->charging && g_st.functional != 1) g_check_at = time(NULL) + VERIFY_SEC;
    return true;
}

static long ago(time_t t, time_t now) { return t ? (long)(now - t) : -1; }

static void on_override(int actual, int expected, const Batt *b, time_t now) {
    char ts[24];
    stamp(now, ts, sizeof ts);
    g_st.overrides++;
    snprintf(g_st.override, sizeof g_st.override,
             "%s value %d (expected %d), %s, %lds since plug change, %lds since wake",
             ts, actual, expected, b->ac ? "on AC" : "on battery", ago(g_ac_change, now), ago(g_wake_at, now));
    logm(LOG_WARNING, "charging key changed externally: %s", g_st.override);
    time_t oldest = g_fights[g_fight_i];
    g_fights[g_fight_i] = now;
    g_fight_i = (g_fight_i + 1) % FIGHT_N;
    if (oldest && oldest <= now && now - oldest < FIGHT_SEC)    // 时钟回拨留下的未来时间不算
        fatal("another program keeps changing the charging key (AlDente? battery?)");
}

static void adapter_set(bool off, int pct, int upper) {
    g_adapter_off = off;
    g_prev_ac = -1;               // 系统报告的供电来源会随之变化，不算插拔
    if (off) note("power adapter disconnected: discharging from %d%% to %d%%", pct, upper - 1);
    else     note("power adapter reconnected at %d%%", pct);
}

// 现在可以断开适配器吗：有键、本系统 + 固件上没被判定无效、没有因写入失败而暂停或停用
static bool discharge_ready(time_t now) {
    return g_adapter && g_st.dfunctional != -1 && g_st.dpause != -1 && g_st.dpause <= now;
}

// 断开失败：记下读回的原始值，恢复供电，暂停 10 分钟后重试；连续失败 3 次则停用，直到 discharge on 或重启
static void discharge_failed(time_t now) {
    char raw[16], ts[24];
    ctrl_raw(g_adapter, raw, sizeof raw);
    if (adapter_restore()) { g_adapter_off = false; g_prev_ac = -1; }
    stamp(now, ts, sizeof ts);
    g_dfail++;
    snprintf(g_st.dfail, sizeof g_st.dfail, "%s %s read back %s after writing %02x (%d/%d)",
             ts, g_adapter->name, raw, g_adapter->on, g_dfail, IO_FAIL_MAX);
    if (g_dfail >= IO_FAIL_MAX) {
        g_st.dpause = -1;
        note("discharge disabled: %s; run 'sudo chargectl discharge on' to retry", g_st.dfail);
    } else {
        g_st.dpause = now + DPAUSE_SEC;
        note("discharge paused for %d min: %s", DPAUSE_SEC / 60, g_st.dfail);
    }
}

// 插电放电：先确认上次断开是否生效，再让适配器键与期望一致
static void adapter_step(const Conf *c, const Batt *b, time_t now) {
    if (!g_adapter) return;

    // 断开后 60 秒，系统应已改用电池；仍报告接着电源 → 键无效，只停用放电
    if (g_dcheck_at && now >= g_dcheck_at) {
        g_dcheck_at = 0;
        if (g_adapter_off && b->ac) {
            g_st.dfunctional = -1;
            if (adapter_restore()) { g_adapter_off = false; g_prev_ac = -1; }
            note("discharge disabled: still on AC after setting %s; run 'sudo chargectl discharge on' to retry",
                 g_adapter->name);
        } else if (g_adapter_off) {
            g_st.dfunctional = 1;
            logm(LOG_NOTICE, "verified: macOS runs on battery when %s is set", g_adapter->name);
        }
    }

    bool want = discharge_ready(now) && policy_discharge(c, &g_st.p, b->pct, g_adapter_off, b->ac);
    if (ctrl_read(g_adapter) == (int)want) { g_adapter_off = want; g_adapter_fail = 0; return; }

    if (ctrl_write(g_adapter, want)) {
        g_adapter_fail = 0;
        if (want) { g_dfail = 0; g_st.dpause = 0; }
        if (want != g_adapter_off) adapter_set(want, b->pct, c->upper);
        g_dcheck_at = want && b->ac && g_st.dfunctional != 1 ? now + VERIFY_SEC : 0;
    } else if (want) {
        discharge_failed(now);
    } else if (++g_adapter_fail >= IO_FAIL_MAX) {
        fatal("cannot reconnect the power adapter");
    }
}

static void tick(bool after_wake) {
    time_t now = time(NULL);
    Conf c = conf_read();
    Batt b = batt_read();
    if (!b.ok) return;
    if (!g_adapter_off) {         // 放电时系统总报告"使用电池"，那段时间无法判断插拔
        if (g_prev_ac >= 0 && b.ac != g_prev_ac) g_ac_change = now;
        g_prev_ac = b.ac;
    }

    const Policy before = g_st.p;
    const int  overrides = g_st.overrides, functional = g_st.functional, dfunctional = g_st.dfunctional;
    const long stall = g_st.stall_since;
    g_noted = false;

    // 时钟回拨：待验证的时间点改为现在（立即验证，而不是取消）；其他未来时间清零
    if (g_check_at > now)        g_check_at = now;
    if (g_dcheck_at > now)       g_dcheck_at = now;
    if (g_st.stall_since > now)  g_st.stall_since = 0;
    if (g_ac_change > now)       g_ac_change = 0;
    if (g_wake_at > now)         g_wake_at = 0;

    Obs o = { b.pct, b.ac, b.charging, now };
    char why[96];
    int ev = policy_step(&c, &g_st.p, &o, g_prev_pct, &g_full_since, why, sizeof why);
    g_prev_pct = b.pct;
    if (ev & EV_CALIB_DONE)  note("calibration done: held 100%% for %d s", CALIB_HOLD_SEC);
    if (ev & EV_TRIGGER)     note("limit reached at %d%% (%d/%d before calibration)", b.pct, g_st.p.triggers, c.calib_every);
    if (ev & EV_CALIB_START) note("calibration started: charging to 100%%");

    int expected = g_written, actual = key_read();
    int ks = key_sync(actual, &g_written, g_st.p.inhibited, after_wake);
    if (ks & KS_OVERRIDE) on_override(actual, expected, &b, now);
    if (ks & KS_WRITE) {
        if (apply(g_st.p.inhibited, &b, why)) g_io_fail = 0;
        else if (++g_io_fail >= IO_FAIL_MAX) fatal("SMC read/write failed 3 times in a row");
    } else {
        g_io_fail = 0;
    }

    // 停充后确认真的不再充电（每个系统 + 固件版本确认一次）
    if (g_check_at && now >= g_check_at) {
        g_check_at = 0;
        if (b.ac && g_written == 1) {
            if (b.charging) {
                g_st.functional = -1;
                fatal("key written but battery is still charging; disabled until 'sudo chargectl on'");
            }
            g_st.functional = 1;
            logm(LOG_NOTICE, "verified: charging stops when %s is set", g_ctrl->name);
        }
    }

    adapter_step(&c, &b, now);

    // 允许充电却一直不充：记一次日志（status 里也会提示）
    if (!charge_stalled(&g_st.p, g_written == 0, &o)) {
        g_st.stall_since = 0;
        g_stall_logged = false;
    } else if (!g_st.stall_since) {
        g_st.stall_since = now;
    } else if (now - g_st.stall_since >= STALL_SEC && !g_stall_logged) {
        g_stall_logged = true;
        if (g_st.p.calibrating)
            note("calibrating: on AC but macOS is not charging at %d%%; if the battery is hot it resumes once "
                 "cool, otherwise use the battery below 80%% then plug in, and turn off Optimized Battery Charging",
                 b.pct);
        else
            note("charging allowed but not charging at %d%%: battery may be hot, or Optimized Battery Charging "
                 "is holding it", b.pct);
    }

    if (g_noted || !policy_eq(&before, &g_st.p) || overrides != g_st.overrides
        || functional != g_st.functional || dfunctional != g_st.dfunctional || stall != g_st.stall_since)
        state_save(&g_st);
}

static io_connect_t g_pm;

static void pm_cb(void *ref, io_service_t s, uint32_t msg, void *arg) {
    (void)ref; (void)s;
    switch (msg) {
        case kIOMessageCanSystemSleep:
            IOAllowPowerChange(g_pm, (long)arg);
            break;
        case kIOMessageSystemWillSleep: {
            // 睡眠中进程不运行：
            //   正在放电 → 恢复适配器供电（不能让它睡着时一直用电池），此时已停充，电量保持不变
            //   正在往上限充 → 先停在当前电量，不改变策略状态，醒来后恢复
            Conf c = conf_read(); Batt b = batt_read();
            if (g_adapter_off) {
                if (!adapter_restore() && !adapter_restore())
                    logm(LOG_ERR, "cannot reconnect the power adapter before sleep");
                else adapter_set(false, b.ok ? b.pct : -1, c.upper);
                g_dcheck_at = 0;
                state_save(&g_st);
            }
            if (b.ok && policy_sleep_inhibit(&c, &g_st.p, b.pct)) {
                if (!apply(true, &b, "going to sleep")) apply(true, &b, "going to sleep (retry)");
                g_check_at = 0;
                state_save(&g_st);
            }
            IOAllowPowerChange(g_pm, (long)arg);   // 写入完成后才允许睡眠
            break;
        }
        case kIOMessageSystemHasPoweredOn:
            g_wake_at = time(NULL);
            tick(true);
            break;
    }
}

static void ps_cb(void *ctx) { (void)ctx; tick(false); }   // 插拔电源、电量变化时立即响应

static dispatch_source_t on_signal(int sig, void (^handler)(void)) {
    signal(sig, SIG_IGN);
    dispatch_source_t s = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, sig, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(s, handler);
    dispatch_resume(s);
    return s;
}

static int run_daemon(void) {
    if (daemon_alive(&g_st)) {   // 另一个实例在工作：以 0 退出，launchd 不会反复重启
        fprintf(stderr, L("守护进程已在运行（pid %d）\n", "The daemon is already running (pid %d)\n"), g_st.pid);
        return 0;
    }
    recover_if_crashed(&g_st);
    if (!adapter_restore()) logm(LOG_ERR, "cannot reconnect the power adapter at startup");
    if (!g_ctrl) fatal("no supported SMC charging key (the firmware may block it)");
    if (fault_blocks()) {
        g_st.running = 0;
        note("not started: the charging key had no effect on this macOS/firmware; run 'sudo chargectl on' to retry");
        state_save(&g_st);
        return 0;
    }

    // 启动验证：写"允许充电"并回读
    if (!key_write(false)) fatal("startup check failed: cannot write/read back the charging key");
    g_written = 0;
    record_verified();            // 系统或固件变了会把 dfunctional 清零
    g_st.dpause = 0;              // 写入失败造成的暂停只在一次运行内有效

    g_st.running = 1;
    g_st.pid = getpid();
    g_st.boot = boot_time();
    Conf c = conf_read();
    if (g_conf_ignored) logm(LOG_WARNING, "ignoring %s: must be root-owned and not writable by others", CONF_PATH);
    if (policy_on_start(&c, &g_st.p, time(NULL))) note("calibration started at startup");
    else note("started, keys %s / %s, macOS %s, firmware %s", g_ctrl->name,
              g_adapter ? g_adapter->name : "no adapter key", g_st.os, g_st.fw);
    state_save(&g_st);

    // 用户主动停止（launchctl bootout、卸载、关机）：恢复适配器供电和充电后退出
    dispatch_source_t s1 = on_signal(SIGTERM, ^{ finish("signal"); });
    dispatch_source_t s2 = on_signal(SIGINT,  ^{ finish("signal"); });
    dispatch_source_t s3 = on_signal(SIGHUP,  ^{ tick(false); });   // chargectl 改配置后发 HUP
    dispatch_source_t s4 = on_signal(SIGUSR1, ^{                     // chargectl on / discharge on：重试放电
        if (!g_adapter || (g_st.dfunctional != -1 && !g_st.dpause)) return;
        if (g_st.dfunctional == -1) g_st.dfunctional = 0;
        g_st.dpause = 0;
        g_dfail = 0;
        note("discharge re-enabled by user");
        state_save(&g_st);
        tick(false);
    });
    (void)s1; (void)s2; (void)s3; (void)s4;

    dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 0), TICK_SEC * NSEC_PER_SEC, 5 * NSEC_PER_SEC);
    dispatch_source_set_event_handler(t, ^{ tick(false); });
    dispatch_resume(t);

    IONotificationPortRef port; io_object_t notifier;
    g_pm = IORegisterForSystemPower(NULL, &port, pm_cb, &notifier);
    if (g_pm) CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(port), kCFRunLoopCommonModes);

    CFRunLoopSourceRef ps = IOPSNotificationCreateRunLoopSource(ps_cb, NULL);
    if (ps) CFRunLoopAddSource(CFRunLoopGetCurrent(), ps, kCFRunLoopDefaultMode);

    CFRunLoopRun();
    return 0;
}

// ---------------- 命令（面向用户的输出：中文 / 英文） ----------------

static void fmt_dur(long s, char *buf, size_t n) {
    if (s < 60)              snprintf(buf, n, "%s", L("不到 1 分钟", "under 1 minute"));
    else if (s >= 2 * 86400) snprintf(buf, n, L("%ld 天", "%ld days"), s / 86400);
    else if (s >= 3600)      snprintf(buf, n, L("%ld 小时", "%ld hours"), s / 3600);
    else                     snprintf(buf, n, L("%ld 分钟", "%ld min"), s / 60);
}

static void fmt_date(time_t t, char *buf, size_t n) { strftime(buf, n, "%Y-%m-%d %H:%M", localtime(&t)); }

static bool os_at_least(int major, int minor) {
    char v[32];
    int a = 0, b = 0;
    sysctl_str("kern.osproductversion", v, sizeof v);
    sscanf(v, "%d.%d", &a, &b);
    return a > major || (a == major && b >= minor);
}

#define INDENT L("             ", "               ")   // 续行与标签后的内容对齐

static int print_status(bool smc) {
    time_t now = time(NULL);
    char ver[32], t1[32], t2[32];
    Build bd = build_now();
    sysctl_str("kern.osproductversion", ver, sizeof ver);
    printf(L("系统       : macOS %s (%s)   固件: %s\n", "System      : macOS %s (%s)   firmware: %s\n"), ver, bd.os, bd.fw);

    printf("%s", L("充电控制键 : ", "Charge key  : "));
    if (!smc)        printf("%s\n", L("无法打开 AppleSMC", "cannot open AppleSMC"));
    else if (g_ctrl) printf("%s\n", g_ctrl->name);
    else if (g_ctrl_diag[0]) printf("%s\n", g_ctrl_diag);
    else printf("%s\n", L("未找到（新固件可能已封锁这些键）", "not found (newer firmware may block these keys)"));
    if (smc && !g_ctrl && os_at_least(26, 4))
        printf("%s%s\n", INDENT, L("可以改用 系统设置 → 电池 → 充电 ⓘ 里的「充电上限」",
                                   "use Charge Limit in System Settings → Battery → Charging ⓘ instead"));

    Batt b = batt_read();
    int a = g_adapter ? ctrl_read(g_adapter) : -1;   // 1 = 适配器供电已断开（正在放电）
    if (b.ok) printf(L("电量       : %d%%   电源: %s   正在充电: %s\n", "Battery     : %d%%   power: %s   charging: %s\n"),
                     b.pct, b.ac ? L("适配器", "adapter")
                          : a == 1 ? L("电池（适配器供电已断开）", "battery (adapter disconnected)") : L("电池", "battery"),
                     b.charging ? L("是", "yes") : L("否", "no"));
    int k = g_ctrl ? key_read() : -1;
    printf(L("键的当前值 : %s\n", "Key value   : %s\n"),
           k == 1 ? L("停止充电", "charging stopped") : k == 0 ? L("允许充电", "charging allowed") : L("未知", "unknown"));

    Conf c = conf_read();
    printf(L("配置       : %s  上限 %d%%  下限 %d%%  校准: ", "Config      : %s  upper %d%%  lower %d%%  calibration: "),
           c.enabled ? L("启用", "on") : L("关闭", "off"), c.upper, c.lower);
    if (c.calib_every && c.calib_days)
        printf(L("每触发上限 %d 次或每 %d 天\n", "every %d limits or %d days\n"), c.calib_every, c.calib_days);
    else if (c.calib_every) printf(L("每触发上限 %d 次\n", "every %d limits\n"), c.calib_every);
    else if (c.calib_days)  printf(L("每 %d 天\n", "every %d days\n"), c.calib_days);
    else                    printf("%s\n", L("关闭", "off"));
    if (g_conf_ignored)
        printf(L("%s%s 属主或权限不安全，已忽略，正在使用默认值\n", "%s%s has unsafe owner/permissions; ignored, using defaults\n"),
               INDENT, CONF_PATH);

    bool alive = daemon_alive(&g_st);   // 暂停 / 停用只在守护进程运行期间有意义
    printf("%s", L("插电放电   : ", "Discharge   : "));
    if (!c.discharge)
        printf("%s\n", L("关闭", "off"));
    else if (!g_adapter)
        printf("%s\n", L("不可用：未找到适配器控制键（CHIE / CH0I）", "unavailable: no adapter key found (CHIE / CH0I)"));
    else if (g_st.dfunctional == -1 && same_build())
        printf("%s\n", L("已停用：断开适配器在本系统和固件上无效（sudo chargectl discharge on 可重试）",
                         "disabled: disconnecting the adapter had no effect on this macOS/firmware "
                         "(retry: sudo chargectl discharge on)"));
    else if (alive && g_st.dpause == -1)
        printf("%s\n", L("已停用：断开适配器连续失败（sudo chargectl discharge on 可重试）",
                         "disabled: disconnecting the adapter kept failing (retry: sudo chargectl discharge on)"));
    else if (alive && g_st.dpause > now) {
        fmt_dur(g_st.dpause - now, t1, sizeof t1);
        printf(L("暂停：断开适配器失败，%s后重试\n", "paused: disconnecting the adapter failed; retry in %s\n"), t1);
    } else if (a == 1)
        printf(L("进行中：已断开适配器供电（%s），放电到 %d%%\n", "in progress: adapter disconnected (%s), discharging to %d%%\n"),
               g_adapter->name, c.upper - 1);
    else
        printf(L("待命（%s）：接着电源且电量高于上限时放电%s\n", "standby (%s): discharges when on AC and above the upper limit%s\n"),
               g_adapter->name, g_st.dfunctional == 1 ? L("，已确认生效", ", confirmed working") : "");
    if (c.discharge && g_st.dfail[0])
        printf(L("%s最近一次失败：%s\n", "%slast failure: %s\n"), INDENT, g_st.dfail);

    printf("%s%s\n", L("守护进程   : ", "Daemon      : "),
           alive ? L("运行中", "running")
           : g_st.running ? L("上次异常退出（下次以 root 运行任何命令时会先复位）",
                              "exited abnormally (charging is reset on the next root command)")
           : fault_blocks()
             ? L("已停用：停充在本系统和固件上无效（sudo chargectl on 可重试）",
                 "disabled: stopping charge had no effect on this macOS/firmware (retry: sudo chargectl on)")
             : L("未运行", "not running"));

    printf(L("策略状态   : %s  已触发上限 %d 次", "Policy      : %s  limit reached %d times"),
           g_st.p.inhibited ? L("停充", "stop charging") : L("允许充电", "allow charging"), g_st.p.triggers);
    if (g_st.p.last_calib) {
        fmt_date(g_st.p.last_calib, t1, sizeof t1);
        printf(L("  上次校准 %s", "  last calibration %s"), t1);
    }
    putchar('\n');

    Obs o = { b.pct, b.ac, b.charging, now };
    bool stalled = b.ok && charge_stalled(&g_st.p, k == 0, &o);
    if (g_st.p.calibrating) {
        printf("%s", L("校准       : ", "Calibration : "));
        if (stalled && g_st.stall_since && now - g_st.stall_since >= STALL_SEC) {
            fmt_dur(now - g_st.stall_since, t2, sizeof t2);
            printf(L("已接电但系统未继续充电（%d%%，已停 %s）\n", "on AC but macOS is not charging (%d%%, for %s)\n"), b.pct, t2);
            printf("%s%s\n", INDENT, L("电池温度过高时，降温后会自动继续；否则请用电到 80% 以下再插电，",
                                       "If the battery is hot, charging resumes once it cools down; otherwise use"));
            printf("%s%s\n", INDENT, L("如果开启了「优化电池充电」，请关闭",
                                       "the battery below 80% and plug in again, and turn off Optimized Battery Charging"));
        } else {
            fmt_dur(g_st.p.calib_since ? now - g_st.p.calib_since : 0, t1, sizeof t1);
            printf(L("等待系统充至 100%%（已持续 %s，当前 %d%%）\n", "waiting for macOS to charge to 100%% (for %s, now %d%%)\n"),
                   t1, b.pct);
        }
    }

    if (!g_st.verified) printf("%s%s\n", L("键验证     : ", "Verified    : "), L("还没有验证过", "not yet"));
    else {
        fmt_date(g_st.verified, t1, sizeof t1);
        printf(L("键验证     : %s（macOS %s，固件 %s，%s）%s\n", "Verified    : %s (macOS %s, firmware %s, %s)%s\n"),
               t1, g_st.os, g_st.fw, g_st.ctrl,
               g_st.functional == 1  ? L("，已确认停充生效", ", stopping charge confirmed")
               : g_st.functional == -1 ? L("，停充无效", ", stopping charge had no effect") : "");
        if (!same_build())
            printf("%s%s\n", INDENT, L("系统或固件已更新，守护进程下次启动时会重新验证",
                                       "macOS or firmware changed; the daemon re-verifies on its next start"));
    }
    if (g_st.overrides)
        printf(L("外部改写   : %d 次，最近一次 %s\n", "Overrides   : %d, latest %s\n"), g_st.overrides, g_st.override);
    if (g_st.last[0])
        printf("%s%s\n", L("最近动作   : ", "Last action : "), g_st.last);
    if (!g_st.p.calibrating && stalled)
        printf("%s%s\n", L("提示       : ", "Hint        : "),
               L("允许充电但没有在充：电池温度可能过高（降温后会自动继续），或被「优化电池充电」拦住",
                 "charging allowed but not charging: the battery may be hot (resumes once cool), "
                 "or Optimized Battery Charging is holding it"));
    return !smc ? 2 : g_ctrl ? 0 : 1;   // 2 = 打不开 AppleSMC，1 = 没有可用的充电控制键
}

static int manual(bool on) {   // 手动测试用；守护进程在运行时会被它覆盖，所以拒绝
    if (daemon_alive(&g_st)) {
        fprintf(stderr, L("守护进程正在运行（pid %d），手动写入会被它覆盖。先运行 sudo chargectl off 或卸载。\n",
                          "The daemon is running (pid %d) and would override this. Run 'sudo chargectl off' or uninstall first.\n"),
                g_st.pid);
        return 1;
    }
    recover_if_crashed(&g_st);
    if (!adapter_restore())       // 手动命令只管充电；适配器一律恢复供电
        fprintf(stderr, "%s\n", L("恢复适配器供电失败", "Failed to reconnect the power adapter"));
    if (!g_ctrl) { fprintf(stderr, "%s\n", L("未找到可用的充电控制键", "No supported charging key found")); return 1; }
    if (on && fault_blocks())
        fprintf(stderr, "%s\n", L("注意：之前已确认停充在本系统和固件上无效",
                                  "Note: stopping charge was found to have no effect on this macOS/firmware"));
    bool ok = key_write(on);
    if (ok && g_st.functional != -1) record_verified();   // 不在这里覆盖"停充无效"的结论
    note("manual %s%s", on ? "inhibit" : "allow", ok ? "" : " FAILED");
    state_save(&g_st);
    puts(ok ? (on ? L("已停止充电", "Charging stopped") : L("已允许充电", "Charging allowed"))
            : L("写入或回读失败", "Write or read-back failed"));
    return !ok;
}

// sudo chargectl on / discharge on 调用：清除停用状态，守护进程下次启动时重新验证。
// functional == -1 只由 fatal() 写入，而 fatal() 写完就退出，所以停用时守护进程一定不在运行。
// 若守护进程在运行，状态文件归它管，这里不改（放电的停用由 chargectl 发 USR1 让守护进程自己清除）。
static int clear_fault(void) {
    if (daemon_alive(&g_st) || (g_st.functional != -1 && g_st.dfunctional != -1)) return 0;
    if (g_st.functional == -1)  g_st.functional = 0;
    if (g_st.dfunctional == -1) g_st.dfunctional = 0;
    note("disabled state cleared by user; will verify again");
    state_save(&g_st);
    return 0;
}

// ---------------- 手动测试：不改配置、不写状态文件 ----------------

// 只读：列出所有已知控制键是否存在、长度、当前的值
static int probe(void) {
    const Ctrl *tables[] = { CHARGE_KEYS, ADAPTER_KEYS };
    for (int t = 0; t < 2; t++)
        for (const Ctrl *c = tables[t]; c->name; c++)
            for (int j = 0; j < 2 && c->keys[j]; j++) {
                const char *k = c->keys[j];
                uint32_t sz = smc_size(k);
                printf("%-5s %s  ", k, t ? L("适配器", "adapter") : L("充电  ", "charge "));
                if (!sz) { printf("%s\n", L("不存在", "missing")); continue; }
                uint8_t b[32];
                printf(L("长度 %u（预期 %u）  值 ", "size %u (expected %u)  value "), sz, c->size);
                if (smc_read(k, sz, b)) for (uint32_t i = 0; i < sz; i++) printf("%02x", b[i]);
                else printf("%s", L("读取失败", "read failed"));
                printf(L("  （%02x = 正常，%02x = %s）\n", "  (%02x = normal, %02x = %s)\n"), c->off, c->on,
                       t ? L("断开供电", "disconnected") : L("停充", "stopped"));
            }
    printf(L("\n将使用：充电 %s，适配器 %s\n", "\nWould use: charge %s, adapter %s\n"),
           g_ctrl ? g_ctrl->name : "-", g_adapter ? g_adapter->name : "-");
    return 0;
}

static volatile sig_atomic_t g_stop;
static void on_stop(int sig) { (void)sig; g_stop = 1; }

static void show(int t, const char *what) {
    Batt b = batt_read();
    int a = ctrl_read(g_adapter);
    printf("%4ds  %-14s  %s  %3d%%  %+5d mA  %s=%s\n", t, what,
           b.ac ? L("系统: 接电  ", "macOS: AC     ") : L("系统: 电池  ", "macOS: battery"),
           b.pct, b.ma, g_adapter->name, a == 1 ? "on" : a == 0 ? "off" : "?");
}

// 断开适配器供电 secs 秒，期间显示系统看到的供电来源和电池电流，到时间或 Ctrl+C 一定恢复。
// secs = 0：只恢复供电（应急用）。
static int adapter_test(int secs) {
    if (!g_adapter) {
        fprintf(stderr, "%s\n", L("未找到可用的适配器控制键（运行 probe 查看原因），不做任何写入",
                                  "No usable adapter key (see 'probe'); nothing written"));
        return 1;
    }
    bool ok = adapter_restore();
    printf(L("恢复适配器供电：%s\n", "Reconnect adapter: %s\n"), ok ? L("成功", "ok") : L("失败", "FAILED"));
    if (secs <= 0 || !ok) return !ok;

    Batt b = batt_read();
    if (!b.ok || !b.ac) { fprintf(stderr, "%s\n", L("请先接上电源再测试", "Plug in the power adapter first")); return 1; }
    if (b.pct < 30)     { fprintf(stderr, "%s\n", L("电量低于 30%，先充一会儿再测试", "Battery below 30%; charge first")); return 1; }
    if (secs > 300) secs = 300;

    struct sigaction sa = { .sa_handler = on_stop };   // 不设 SA_RESTART：sleep 会被中断
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    printf(L("将断开适配器供电 %d 秒，Ctrl+C 可提前结束，结束时一定恢复。\n\n",
             "Disconnecting the adapter for %d s; Ctrl+C ends early; it is always reconnected.\n\n"), secs);
    show(0, L("断开前", "before"));
    logm(LOG_NOTICE, "adaptertest: disconnecting adapter via %s for %d s", g_adapter->name, secs);
    bool wrote = ctrl_write(g_adapter, true);
    bool on_battery = false;
    int min_ma = 0;
    for (int t = 1; wrote && t <= secs && !g_stop; t++) {
        sleep(1);
        Batt n = batt_read();
        if (!n.ac) on_battery = true;
        if (n.ma < min_ma) min_ma = n.ma;
        if (t % 5 == 0 || t == secs) show(t, L("已断开", "disconnected"));
        if (n.ok && n.pct < 25) { printf("%s\n", L("电量低于 25%，提前结束", "Below 25%; stopping early")); break; }
    }

    ok = adapter_restore() || adapter_restore();
    logm(LOG_NOTICE, "adaptertest: adapter reconnected%s", ok ? "" : " FAILED");
    sleep(3);
    show(secs, L("恢复后", "after"));

    printf("\n%s", L("结果：", "Result: "));
    if (!wrote)
        printf("%s\n", L("写入或回读失败，键可能不接受这个值", "write/read-back failed; the key may not accept this value"));
    else if (on_battery && min_ma < -100)
        printf(L("生效。系统改用电池，放电电流最大 %d mA\n", "works. macOS switched to battery, discharge up to %d mA\n"), -min_ma);
    else if (on_battery)
        printf("%s\n", L("系统改用电池，但几乎没有放电电流，结果不明确（时间太短？）",
                         "macOS switched to battery but with almost no current; unclear (too short?)"));
    else
        printf("%s\n", L("无效。写入后系统仍报告接着电源", "no effect. macOS still reported AC after the write"));
    if (!ok)
        printf("%s\n", L("警告：恢复供电失败！请再运行 sudo ./chargelimitd adaptertest 0",
                         "WARNING: reconnect failed! Run sudo ./chargelimitd adaptertest 0 again"));
    return !ok;
}

int main(int argc, char **argv) {
    // HUP / USR1 的默认动作是终止进程。守护进程刚启动、还没装好处理器时 chargectl 就可能发来，
    // 所以一开始就忽略，之后由 dispatch source 接管
    signal(SIGHUP, SIG_IGN);
    signal(SIGUSR1, SIG_IGN);

    const char *cmd = argc > 1 ? argv[1] : "status";
    lang_init();
    openlog("chargelimitd", LOG_PID, LOG_DAEMON);
    bool smc = smc_open();
    if (smc) {
        g_ctrl = ctrl_detect(CHARGE_KEYS, g_ctrl_diag, sizeof g_ctrl_diag);
        g_adapter = ctrl_detect(ADAPTER_KEYS, NULL, 0);
    }
    state_load(&g_st);

    if (!strcmp(cmd, "status")) return print_status(smc);
    if (!strcmp(cmd, "probe")) {
        if (!smc) { fprintf(stderr, "%s\n", L("无法打开 AppleSMC", "Cannot open AppleSMC")); return 2; }
        return probe();
    }

    if (geteuid() != 0) { fprintf(stderr, "%s\n", L("需要 root（sudo）", "Must be run as root (sudo)")); return 1; }
    if (!smc) { fprintf(stderr, "%s\n", L("无法打开 AppleSMC", "Cannot open AppleSMC")); return 1; }
    if (!strcmp(cmd, "run"))        return run_daemon();
    if (!strcmp(cmd, "inhibit"))    return manual(true);
    if (!strcmp(cmd, "allow"))      return manual(false);
    if (!strcmp(cmd, "clearfault")) return clear_fault();
    if (!strcmp(cmd, "adaptertest")) return adapter_test(argc > 2 ? atoi(argv[2]) : 60);

    fprintf(stderr, "%s: chargelimitd status | probe | run | inhibit | allow | clearfault | adaptertest [s]\n",
            L("用法", "Usage"));
    return 1;
}
