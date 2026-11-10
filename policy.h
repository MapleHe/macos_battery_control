// policy.h — 充电决策（纯逻辑，不依赖 macOS，可在任何平台上单元测试）
#pragma once
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#define CALIB_HOLD_SEC 300      // 接电并保持 100% 满 5 分钟 → 校准完成
#define DAY_SEC        86400

typedef struct {
    bool enabled;
    int  upper, lower;
    int  calib_every;           // 触发上限多少次后校准；0 = 不按次数
    int  calib_days;            // 距上次校准多少天后校准；0 = 不按天数
} Conf;

// 需要跨重启保存的策略状态
typedef struct {
    bool   inhibited;           // 策略期望：true = 停止充电
    bool   calibrating;         // 校准模式（只由程序自动进入/退出）
    int    triggers;            // 自上次校准以来触发上限的次数
    time_t last_calib;          // 上次保持 100% 满 5 分钟的时间
    time_t calib_since;         // 本次校准开始的时间
} Policy;

typedef struct { int pct; bool ac, charging; time_t now; } Obs;

enum { EV_TRIGGER = 1, EV_CALIB_START = 2, EV_CALIB_DONE = 4 };

static inline Conf conf_sanitize(Conf c) {
    if (c.upper > 100) c.upper = 100;
    if (c.upper < 20)  c.upper = 20;
    if (c.lower < 5)   c.lower = 5;
    if (c.lower > c.upper - 5) c.lower = c.upper - 5;
    if (c.calib_every < 0)   c.calib_every = 0;
    if (c.calib_every > 50)  c.calib_every = 50;
    if (c.calib_days < 0)    c.calib_days = 0;
    if (c.calib_days > 365)  c.calib_days = 365;
    return c;
}

static inline bool policy_eq(const Policy *a, const Policy *b) {
    return a->inhibited == b->inhibited && a->calibrating == b->calibrating && a->triggers == b->triggers
        && a->last_calib == b->last_calib && a->calib_since == b->calib_since;
}

static inline bool due_by_count(const Conf *c, const Policy *p) {
    return c->calib_every > 0 && p->triggers >= c->calib_every;
}

static inline bool due_by_time(const Conf *c, const Policy *p, time_t now) {
    return c->calib_days > 0 && p->last_calib > 0 && now - p->last_calib >= (time_t)c->calib_days * DAY_SEC;
}

static inline int calib_start(Policy *p, time_t now) {
    p->calibrating = true;
    p->inhibited = false;
    p->calib_since = now;
    return EV_CALIB_START;
}

// 时钟回拨后，记录里的未来时间不可信：
//   last_calib 在未来 → 改为 now，并让满电重新计时（否则 last_calib <= full_since 永远不成立，校准结束不了）
//   full_since 在未来 → 清零，下个周期重新计时
static inline void clock_guard(Policy *p, time_t now, time_t *full_since) {
    if (p->calib_since > now) p->calib_since = now;
    if (p->last_calib > now) { p->last_calib = now; if (full_since) *full_since = 0; }
    if (full_since && *full_since > now) *full_since = 0;
}

// 启动时：第一次运行从现在开始计天数；任一校准条件已满足则进入校准
static inline int policy_on_start(const Conf *c, Policy *p, time_t now) {
    clock_guard(p, now, NULL);
    if (!p->last_calib) p->last_calib = now;
    if (!c->enabled || p->calibrating) return 0;
    return due_by_count(c, p) || due_by_time(c, p, now) ? calib_start(p, now) : 0;
}

// 一次决策，更新 p，返回事件位。
//   prev_pct   本进程上一次读到的电量；-1 = 未知（刚启动，不计入触发次数）
//   full_since 接电且连续 100% 的起始时间；0 = 当前不在 100%
// 次数条件只在"到达上限"时检查（限制 N 次后，下一次充到 100%）；天数条件每次都检查，
// 因为一直插着电时电量停在上限，不会再"到达上限"。
static inline int policy_step(const Conf *c, Policy *p, const Obs *o, int prev_pct,
                              time_t *full_since, char *why, size_t n) {
    int ev = 0;
    clock_guard(p, o->now, full_since);

    // 接电保持 100% 满 5 分钟就算校准过一次，不论是谁让它充满的；每次满电只记一次
    if (!(o->ac && o->pct >= 100)) *full_since = 0;
    else if (!*full_since) *full_since = o->now;
    else if (o->now - *full_since >= CALIB_HOLD_SEC && p->last_calib <= *full_since) {
        if (p->calibrating) ev |= EV_CALIB_DONE;
        p->calibrating = false;
        p->triggers = 0;
        p->last_calib = o->now;
        p->calib_since = 0;
    }

    if (!c->enabled) {
        p->inhibited = false;
        snprintf(why, n, "limiter disabled");
        return ev;
    }
    if (!p->calibrating && due_by_time(c, p, o->now)) ev |= calib_start(p, o->now);
    if (p->calibrating) {
        p->inhibited = false;
        snprintf(why, n, "calibrating: %d%%, charging to 100%%", o->pct);
        return ev;
    }
    if (o->pct >= c->upper) {
        if (!p->inhibited) {                      // 只在"允许 → 停止"的那一刻计数或进入校准
            if (due_by_count(c, p)) {
                snprintf(why, n, "calibration started after %d limits", p->triggers);
                return ev | calib_start(p, o->now);
            }
            if (prev_pct >= 0 && prev_pct < c->upper) { p->triggers++; ev |= EV_TRIGGER; }
            p->inhibited = true;
        }
        snprintf(why, n, "%d%% >= upper %d%%", o->pct, c->upper);
    } else if (o->pct <= c->lower) {
        p->inhibited = false;
        snprintf(why, n, "%d%% <= lower %d%%", o->pct, c->lower);
    } else {
        snprintf(why, n, "%d%% between limits, hold", o->pct);
    }
    return ev;
}

// 键的实际值和本进程最后写入值的对账。
//   actual  读到的值：1 = 停止充电，0 = 允许，-1 = 读取失败
//   written 本进程最后写入的值；-1 = 未知（刚启动或上次写入失败）。函数内同步为 actual。
//   after_wake 睡眠期间固件可能重置键，这次的不一致不算外部改写
enum { KS_OVERRIDE = 1, KS_READ_FAIL = 2, KS_WRITE = 4 };

static inline int key_sync(int actual, int *written, bool desired, bool after_wake) {
    if (actual < 0) return KS_READ_FAIL | KS_WRITE;
    int r = 0;
    if (*written >= 0 && actual != *written && !after_wake) r |= KS_OVERRIDE;
    *written = actual;
    if (actual != (int)desired) r |= KS_WRITE;
    return r;
}

// 我们允许充电、接着电源、电量未到阈值，却没在充：被系统拦住，或系统暂不充电。
// 校准期间要到 100%，所以阈值是 100；平时到 95% 以上不充是正常现象。
static inline bool charge_stalled(const Policy *p, bool key_allows, const Obs *o) {
    return key_allows && o->ac && !o->charging && o->pct < (p->calibrating ? 100 : 95);
}

// 即将睡眠：睡眠中进程不运行，正在往上限充的话先停在当前电量（醒来后恢复）
static inline bool policy_sleep_inhibit(const Conf *c, const Policy *p, int pct) {
    return c->enabled && !p->calibrating && !p->inhibited && pct > c->lower;
}
