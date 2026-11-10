// 测试 policy.h 的决策逻辑。不访问 SMC 和电池，任何平台都能运行：
//   cc -Wall -Wextra -o test_policy tests/test_policy.c && ./test_policy
#include <assert.h>
#include <string.h>
#include "../policy.h"

static Conf   C;
static Policy P;
static int    prev;
static time_t T, full;

static void reset(void) {
    C = (Conf){ .enabled = true, .upper = 80, .lower = 60, .calib_every = 3, .calib_days = 30, .discharge = true };
    memset(&P, 0, sizeof P);
    prev = -1; full = 0; T = 1000000;
    policy_on_start(&C, &P, T);
}

static int step(int pct, bool ac) {
    char why[96];
    Obs o = { pct, ac, ac && pct < 100, T };
    int ev = policy_step(&C, &P, &o, prev, &full, why, sizeof why);
    prev = pct;
    T += 30;
    return ev;
}

// 从 55% 充到上限，再用电回到下限
static void cycle(void) {
    for (int p = 55; p < 80; p++) { step(p, true); assert(!P.inhibited); }
    step(80, true);
    assert(P.inhibited || P.calibrating);
    if (P.calibrating) return;
    for (int p = 79; p > 60; p--) { step(p, false); assert(P.inhibited); }   // 中间保持
    step(60, false);
    assert(!P.inhibited);
}

// 在 100% 接电保持 n 个周期（每个 30 s）
static int hold_full(int n) { int ev = 0; while (n--) ev |= step(100, true); return ev; }

int main(void) {
    // 第一次运行：last_calib 初始化为现在，不会立刻校准
    reset();
    assert(P.last_calib == T && !P.calibrating);

    // 启动时就在上限以上：停充，但不计入触发次数
    assert(step(85, true) == 0 && P.inhibited && P.triggers == 0);

    // 三次正常循环各计一次；第 4 次到达上限进入校准，继续充
    reset();
    step(55, true);
    for (int i = 1; i <= 3; i++) { cycle(); assert(P.triggers == i && !P.calibrating); }
    for (int p = 61; p < 80; p++) step(p, true);
    assert(step(80, true) == EV_CALIB_START && P.calibrating && !P.inhibited && P.calib_since);
    for (int p = 81; p < 100; p++) { step(p, true); assert(!P.inhibited && P.calibrating); }

    // 100% 不满 5 分钟不结束；中途拔电重新计时；满 300 s 结束，随即停充且不计数
    assert(hold_full(5) == 0 && P.calibrating);
    step(100, false);
    assert(hold_full(10) == 0 && P.calibrating);           // 270 s
    time_t done_at = T;
    assert(hold_full(1) == EV_CALIB_DONE);
    assert(!P.calibrating && P.triggers == 0 && P.inhibited && P.last_calib == done_at && !P.calib_since);

    // 同一次满电只记一次（last_calib 不再每个周期更新）
    hold_full(20);
    assert(P.last_calib == done_at);

    // 按天数：一直插电停在上限，满 30 天的那个周期进入校准
    reset();
    step(79, true); step(80, true);
    assert(P.inhibited && P.triggers == 1);
    T = P.last_calib + 30 * DAY_SEC - 30;
    assert(step(80, true) == 0 && P.inhibited);
    assert(step(80, true) == EV_CALIB_START && P.calibrating && !P.inhibited);

    // 校准一直没充满（拔线、被系统拦住）：保持校准，不设超时
    for (int i = 0; i < 5000; i++) step(i % 2 ? 96 : 90, i % 3 != 0);
    assert(P.calibrating);

    // 启动时任一条件已满足：直接进入校准；校准状态不受配置改动影响
    reset();
    P.triggers = 3; P.inhibited = true;
    assert(policy_on_start(&C, &P, T) == EV_CALIB_START && P.calibrating && !P.inhibited);
    C.calib_every = 0; C.calib_days = 0;
    step(90, true);
    assert(P.calibrating && !P.inhibited);
    reset();
    T += 31 * DAY_SEC;
    assert(policy_on_start(&C, &P, T) == EV_CALIB_START);

    // 时钟回拨：last_calib 在未来 → 不按天数触发，改为现在
    reset();
    P.last_calib = T + 10 * DAY_SEC;
    assert(step(70, true) == 0 && !P.calibrating && P.last_calib == T - 30);
    // last_calib 在未来、且已有一个过去的 full_since：满电重新计时，校准照样能完成
    reset();
    P.calibrating = true; P.calib_since = T + DAY_SEC; P.last_calib = T + 10 * DAY_SEC; full = T - 100;
    assert(hold_full(11) == EV_CALIB_DONE && !P.calibrating);
    // full_since 在未来：清零重新计时，满电 5 分钟后完成
    reset();
    P.calibrating = true; full = T + 1000;
    assert(hold_full(10) == 0 && P.calibrating);
    assert(hold_full(1) == EV_CALIB_DONE);
    // 启动时 last_calib 在未来：改为现在，不会立刻校准
    reset();
    P.last_calib = T + 5 * DAY_SEC;
    assert(policy_on_start(&C, &P, T) == 0 && P.last_calib == T);

    // 两个条件都关闭：永不进入校准
    reset(); C.calib_every = 0; C.calib_days = 0;
    step(55, true);
    for (int i = 0; i < 6; i++) cycle();
    T += 400 * DAY_SEC;
    step(70, true);
    assert(!P.calibrating && P.triggers == 6);

    // 关闭限制：允许充电；充满 5 分钟也清零计数并更新 last_calib
    reset(); C.enabled = false; P.triggers = 2;
    hold_full(11);
    assert(P.triggers == 0 && P.last_calib > 1000000 && !P.inhibited);

    // 睡眠：只在"允许并且高于下限"时临时停充
    reset();
    assert(policy_sleep_inhibit(&C, &P, 70));
    assert(!policy_sleep_inhibit(&C, &P, 50));
    P.calibrating = true;
    assert(!policy_sleep_inhibit(&C, &P, 70));

    // key_sync：外部改写只计一次，之后同步
    int w = 1;
    assert(key_sync(0, &w, true, false) == (KS_OVERRIDE | KS_WRITE) && w == 0);
    // 外部改成的值恰好等于期望值：计一次，不写；下一次不再重复计数
    w = 1;
    assert(key_sync(0, &w, false, false) == KS_OVERRIDE && w == 0);
    assert(key_sync(0, &w, false, false) == 0);
    // 睡眠前临时停充（w=1），睡眠中固件重置为 0：醒来不计数，并同步
    w = 1;
    assert(key_sync(0, &w, false, true) == 0 && w == 0);
    assert(key_sync(0, &w, false, false) == 0);
    // 读取失败：不算外部改写，需要重写
    w = 1;
    assert(key_sync(-1, &w, true, false) == (KS_READ_FAIL | KS_WRITE) && w == 1);
    // 写入失败后 w=-1（未知）：下次读到任何值都不算外部改写
    w = -1;
    assert(key_sync(1, &w, false, false) == KS_WRITE && w == 1);

    // 该充却不充：平时 95% 以下才算；校准期间 100% 以下都算
    Policy np = {0}, cp = { .calibrating = true };
    Obs o = { 96, true, false, 0 };
    assert(!charge_stalled(&np, true, &o) && charge_stalled(&cp, true, &o));
    o.pct = 70;
    assert(charge_stalled(&np, true, &o) && !charge_stalled(&np, false, &o));
    o.charging = true;
    assert(!charge_stalled(&cp, true, &o));

    // 插电放电：接电且 > 上限才开始；开始后降到上限 - 1 才停；回弹到上限不再触发
    reset();
    assert(!policy_discharge(&C, &P, 80, false, true));
    assert( policy_discharge(&C, &P, 95, false, true));
    assert(!policy_discharge(&C, &P, 95, false, false));     // 没接电不开始
    assert( policy_discharge(&C, &P, 80, true,  false));     // 进行中不看 ac，到 80 还继续
    assert(!policy_discharge(&C, &P, 79, true,  false));     // 79 停
    assert(!policy_discharge(&C, &P, 80, false, true));      // 回弹到 80 不再开始
    // 校准、关闭限制、关闭放电：都不放电（进行中的也要停）
    P.calibrating = true;
    assert(!policy_discharge(&C, &P, 95, true, true));
    P.calibrating = false; C.enabled = false;
    assert(!policy_discharge(&C, &P, 95, true, true));
    C.enabled = true; C.discharge = false;
    assert(!policy_discharge(&C, &P, 95, true, true));
    // 校准完成后（100%，已停充）开始放电回到上限
    reset();
    P.calibrating = true;
    hold_full(11);
    assert(!P.calibrating && P.inhibited && policy_discharge(&C, &P, 100, false, true));

    // 配置边界
    Conf b = conf_sanitize((Conf){ true, 150, 99, 80, 999, true });
    assert(b.upper == 100 && b.lower == 95 && b.calib_every == 50 && b.calib_days == 365);

    puts("policy tests passed");
    return 0;
}
