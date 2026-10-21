#include "heart.h"
#include "../SW_FreeRTOS/SW_FreeRTOS.h"
#include "MAX30105.h"
#include "spo2_algorithm.h"
#include <string.h>
#include <math.h>

/* ============================================================================
 * 一、参数配置
 * ----------------------------------------------------------------------------
 * ★ 有效采样率 = HEART_SAMPLE_RATE / HEART_SAMPLE_AVG，必须等于算法里的 FreqS。
 *   spo2_algorithm.h 写死 FreqS = 25、BUFFER_SIZE = FreqS*4 = 100。
 *   所以这里必须是 100/4 = 25 Hz，窗口 100 点 = 4 秒。
 * ==========================================================================*/
#define HEART_LED_BRIGHTNESS  0x7F   // ≈25 mA，18 bit 动态范围足够，不会饱和
#define HEART_SAMPLE_AVG      4
#define HEART_LED_MODE        2      // 1=Red 2=Red+IR 3=Red+IR+Green
#define HEART_SAMPLE_RATE     100    // 100 ÷ 4 = 25 Hz ← 与 FreqS 对齐
#define HEART_PULSE_WIDTH     411    // 18 bit，信噪比最好
#define HEART_ADC_RANGE       4096

#define HEART_WIN             BUFFER_SIZE            // 100 点 = 4 秒
#define HEART_STEP            25                     // 每轮滑动 25 点 = 1 秒
#define HEART_EFFECTIVE_HZ    (HEART_SAMPLE_RATE / HEART_SAMPLE_AVG)   // 25

/* ----------------------------------------------------------------------------
 * 手指在位判定：自适应基线 + 迟滞
 * ----------------------------------------------------------------------------
 * 旧版用固定阈值（进入 4000 / 退出 1500），一旦手表背壳自身的漏光（LED 光线
 * 经盖板折射回光电二极管，即光学串扰）超过 1500，就会出现：
 *   · 手指早已拿开，IR 仍高于退出阈值 → fingerOn 永远不释放，
 *     窗口继续被噪声填充、界面上的数值一直自己在变；
 *   · 反过来阈值定得太高，手指按得轻一点又永远进不去判定。
 * 现在改成：基线 irBase 在「判定没有手指」期间以很慢的速度跟踪实测 IR，
 * 门限随之浮动，串扰电平会被自动吸收。
 * --------------------------------------------------------------------------*/
#define HEART_IR_ENTER_ABS    3000    // 进入门限的绝对下限
#define HEART_IR_EXIT_ABS     1500    // 退出门限的绝对下限
#define HEART_IR_ENTER_RATIO  1.80f   // 高于基线 1.8 倍才算有手指
#define HEART_IR_EXIT_RATIO   1.25f   // 回落到基线 1.25 倍以内算手指离开
#define HEART_BASE_ALPHA      0.020f  // 基线跟踪速度（≈50 个样本 = 2 秒）
#define HEART_BASE_MIN        50.0f
#define HEART_BASE_MAX        80000.0f
#define HEART_EMA_ALPHA       0.30f   // 判定用电平的滑动平均系数（≈3 个样本）

// 信号质量门限：交流分量太小就根本不是脉搏，是噪声
#define HEART_AC_MIN_ABS      15      // 窗口 IR 峰峰值的绝对下限
#define HEART_AC_MIN_REL      0.0015f // 峰峰值至少要占直流的 0.15%

#define HEART_MIN_HR          40
#define HEART_MAX_HR          200
#define HEART_MIN_SPO2        70
#define HEART_MAX_SPO2        100
#define HEART_MED_WIN         5       // 中值滤波窗口
#define HEART_STABLE_NEED     1       // 连续有效读数达到该次数才判定“读数正常”
#define HEART_MISS_TOLERATE   3       // 连续无效超过该次数则清零显示

#define HEART_POLL_MS         20      // 任务轮询间隔（25 Hz 采样，20 ms 内最多积 1 个样本）
#define HEART_BATCH_MAX       8       // 单轮最多取走的样本数（库内部缓冲实际只有 3 个）
#define HEART_STARVE_LIMIT    150     // 连续空转次数（150 × 20 ms = 3 秒）判定传感器掉线
#define HEART_FALSE_POS_MS    20000   // 判定“有手指”超过这么久却始终无有效读数 → 认为误判

/* ----------------------------------------------------------------------------
 * LED 占空比控制（降低发热的关键）
 * ----------------------------------------------------------------------------
 * MAX30105 的红外 + 红光两路 LED 是整块板上最热的元件，而且紧贴皮肤。
 * 「没放手指」时点亮 LED 毫无意义，所以：
 *   · 手指离开超过 HEART_LED_IDLE_MS 后把 LED 电流置 0；
 *   · 之后每 HEART_LED_PROBE_PERIOD 打开一次 LED、持续 HEART_LED_PROBE_ON，
 *     仅用这个窗口做「手指是否放上」的探测；
 *   · 一旦探到手指立刻恢复常亮，测量信噪比不受影响。
 * 探测窗口从 200 ms 加长到 400 ms（≈10 个样本）：200 ms 时去掉 LED 电流
 * 突变的暖机样本后只剩 1~2 个样本进判定，手指按得轻一点要等好几个窗口才认；
 * 400 ms 下 3~4 个样本就能判定完，平均功耗仍只有常亮的约 1/4。
 * --------------------------------------------------------------------------*/
#define HEART_LED_IDLE_MS      4000    // 连续这么久没探到手指 → 转入低频探测
#define HEART_LED_PROBE_PERIOD 1500    // 探测周期
#define HEART_LED_PROBE_ON      400    // 每次点灯时长
#define HEART_LED_WARMUP_SAMPLES  2    // 点灯后丢弃的前几个样本（LED 电流突变）

/* ============================ 内部状态 ============================ */
static MAX30105 sensor;
static bool     hOnline  = false;
static uint8_t  state    = HEART_ST_OFFLINE;

static uint32_t irBuf [HEART_WIN];
static uint32_t redBuf[HEART_WIN];
static uint16_t samplesIn = 0;

static float    irEma    = 0.0f;   // IR 滑动平均：手指在位判定用
static float    irBase   = 0.0f;   // 无手指时的自适应基线
static bool     fingerOn = false;

static int32_t  outHR   = 0;
static int32_t  outSpO2 = 0;
static int32_t  outIR   = 0;
static int32_t  winDC   = 0;       // 窗口 IR 直流均值
static int32_t  winAC   = 0;       // 窗口 IR 峰峰值
static uint8_t  missCnt = 0;       // 连续无效计数
static uint8_t  okCnt   = 0;       // 连续有效计数
static uint16_t starve  = 0;       // 连续拿不到样本的轮数

// 去重：相邻的 IR+Red 完全一样说明读到了同一个样本
static uint32_t lastIR = 0, lastRed = 0;
static bool     lastValid = false;

// LED 占空比调度
static bool     ledOn          = false;
static uint32_t lastFingerMs   = 0;
static uint32_t fingerSinceMs  = 0;   // 本次“检测到手指”的起始时刻
static uint8_t  ledWarmupLeft  = 0;

// 中值滤波环形缓冲
static int32_t hrRing[HEART_MED_WIN];
static int32_t spRing[HEART_MED_WIN];
static uint8_t hrRingN = 0, spRingN = 0;
static uint8_t hrRingIdx = 0, spRingIdx = 0;

/* ============================ 小工具 ============================ */
static int32_t medianOf(const int32_t *src, uint8_t n) {
    if (n == 0) return 0;
    int32_t t[HEART_MED_WIN];
    for (uint8_t i = 0; i < n; i++) t[i] = src[i];
    for (uint8_t i = 1; i < n; i++) {          // 插入排序，n ≤ 5
        int32_t v = t[i];
        int8_t  j = (int8_t)i - 1;
        while (j >= 0 && t[j] > v) { t[j + 1] = t[j]; j--; }
        t[j + 1] = v;
    }
    return t[n / 2];
}

static void pushRing(int32_t *buf, uint8_t &n, uint8_t &idx, int32_t v) {
    buf[idx] = v;
    idx = (uint8_t)((idx + 1) % HEART_MED_WIN);
    if (n < HEART_MED_WIN) n++;
}

static void resetWindow(void) {
    samplesIn = 0;
    hrRingN   = 0;  spRingN   = 0;
    hrRingIdx = 0;  spRingIdx = 0;
    missCnt   = 0;
    okCnt     = 0;
}

static void clearOutput(void) {
    outHR = 0; outSpO2 = 0;
    hrRingN = 0; spRingN = 0;
    set_var_health_heartbeat(0);
    set_var_health_oxygen(0);
}

/* 当前门限（会随基线浮动） */
static float enterThreshold(void) {
    float t = irBase * HEART_IR_ENTER_RATIO;
    return (t > (float)HEART_IR_ENTER_ABS) ? t : (float)HEART_IR_ENTER_ABS;
}
static float exitThreshold(void) {
    float t = irBase * HEART_IR_EXIT_RATIO;
    return (t > (float)HEART_IR_EXIT_ABS) ? t : (float)HEART_IR_EXIT_ABS;
}

/* 基线只在「判定没有手指」时以很慢的速度跟踪，避免把手指的直流电平学成基线 */
static void updateBaseline(float ir) {
    if (fingerOn) return;
    irBase += (ir - irBase) * HEART_BASE_ALPHA;
    if (irBase < HEART_BASE_MIN) irBase = HEART_BASE_MIN;
    if (irBase > HEART_BASE_MAX) irBase = HEART_BASE_MAX;
}

/* ============================================================================
 * 二、备用算法
 * ----------------------------------------------------------------------------
 * Maxim 官方算法对信号质量相当挑剔：归一化后的固定阈值一旦没被跨过，就会把
 * 整窗判为无效。为了不让界面永远僵在「测量中」，这里补一套自研算法兜底。
 * 但兜底算法**必须够严格** —— 否则没有手指时纯噪声也能算出一个「心率」，
 * 界面上的数字就会自己在变（这正是本次要修掉的现象之一）。
 * ==========================================================================*/

/* 峰值检测法测心率：去均值 → 3 点平滑 → 上升沿跨阈 → 峰间距求平均 */
static bool simpleHR(const uint32_t *ir, int32_t *hrOut) {
    static float s[HEART_WIN];

    double mean = 0.0;
    for (uint16_t i = 0; i < HEART_WIN; i++) mean += ir[i];
    mean /= HEART_WIN;
    if (mean < 2500.0) return false;               // 直流太低 → 根本没有手指

    for (uint16_t i = 0; i < HEART_WIN; i++) {
        float a = (float)ir[i > 0 ? i - 1 : 0];
        float b = (float)ir[i];
        float c = (float)ir[i < HEART_WIN - 1 ? i + 1 : HEART_WIN - 1];
        s[i] = (a + b + c) / 3.0f;
    }

    float mn = s[0], mx = s[0];
    for (uint16_t i = 1; i < HEART_WIN; i++) {
        if (s[i] < mn) mn = s[i];
        if (s[i] > mx) mx = s[i];
    }
    float amp = mx - mn;
    if (amp < 30.0f) return false;                  // 交流分量太小
    if (amp < mean * HEART_AC_MIN_REL) return false; // 相对幅度不到 0.15% → 是噪声

    const float th = mn + amp * 0.55f;              // 上升沿 55% 处作为心动参考点

    int      lastIdx = -1;
    int      gN      = 0;
    float    gaps[16];
    for (uint16_t i = 1; i < HEART_WIN; i++) {
        if (s[i - 1] < th && s[i] >= th) {
            if (lastIdx >= 0 && gN < 16) {
                int gap = (int)i - lastIdx;                 // 25 Hz：1 点 = 40 ms
                if (gap >= 7 && gap <= 45) gaps[gN++] = (float)gap;  // 33~214 bpm
            }
            lastIdx = (int)i;
        }
    }
    if (gN < 2) return false;                       // ★ 至少两个心动周期

    float sum = 0.0f;
    for (int i = 0; i < gN; i++) sum += gaps[i];
    float avg = sum / (float)gN;

    // ★ 节律一致性：峰间距忽长忽短说明这不是心跳而是噪声 / 手指在动
    float dev = 0.0f;
    for (int i = 0; i < gN; i++) { float d = gaps[i] - avg; dev += d * d; }
    dev = sqrtf(dev / (float)gN);
    if (dev > avg * 0.25f) return false;

    int32_t hr = (int32_t)(60.0f * HEART_EFFECTIVE_HZ / avg + 0.5f);
    if (hr < HEART_MIN_HR || hr > HEART_MAX_HR) return false;

    *hrOut = hr;
    return true;
}

/* R 值法测血氧：R = (ACred/DCred) / (ACir/DCir)，SpO2 ≈ 110 − 25R */
static bool simpleSpO2(const uint32_t *ir, const uint32_t *red, int32_t *spOut) {
    double irDC = 0.0, redDC = 0.0;
    for (uint16_t i = 0; i < HEART_WIN; i++) { irDC += ir[i]; redDC += red[i]; }
    irDC  /= HEART_WIN;
    redDC /= HEART_WIN;
    if (irDC < 3000.0 || redDC < 800.0) return false;   // 直流太低 → 没有手指

    double irAC = 0.0, redAC = 0.0;
    for (uint16_t i = 0; i < HEART_WIN; i++) {
        double a = (double)ir[i]  - irDC;
        double b = (double)red[i] - redDC;
        irAC  += a * a;
        redAC += b * b;
    }
    irAC  = sqrt(irAC  / HEART_WIN);            // RMS 交流分量
    redAC = sqrt(redAC / HEART_WIN);
    if (irAC < 1.0) return false;

    double irRatio = irAC / irDC;
    if (irRatio < HEART_AC_MIN_REL) return false;   // 相对脉动不足 → 不是脉搏

    double R = (redAC / redDC) / irRatio;
    if (R < 0.4 || R > 1.5) return false;       // 超出这个范围说明信号本身不可信

    double spo2 = 110.0 - 25.0 * R;
    if (spo2 > 100.0) spo2 = 100.0;
    if (spo2 < (double)HEART_MIN_SPO2) return false;

    *spOut = (int32_t)(spo2 + 0.5);
    return true;
}

/* ============================================================================
 * 三、单窗计算
 * ==========================================================================*/
static void computeOnce(void) {
    // ---- 窗口信号质量：直流均值 / 峰峰值 ----
    uint32_t mn = irBuf[0], mx = irBuf[0];
    uint64_t sum = 0;
    for (uint16_t i = 0; i < HEART_WIN; i++) {
        if (irBuf[i] < mn) mn = irBuf[i];
        if (irBuf[i] > mx) mx = irBuf[i];
        sum += irBuf[i];
    }
    winDC = (int32_t)(sum / HEART_WIN);
    winAC = (int32_t)(mx - mn);

    /* ★ 二次保险：窗口直流电平掉回退出门限以下 → 手指已经离开。
     *   这条判断独立于滑动平均 irEma，能第一时间把状态和数值一起清掉。 */
    if ((float)winDC < exitThreshold()) {
        fingerOn = false;
        resetWindow();
        state = HEART_ST_NO_FINGER;
        clearOutput();
        return;
    }

    // 交流分量太小 → 不可能是脉搏，直接判无效，不给任何数值
    bool acOk = ((float)winAC >= HEART_AC_MIN_ABS) &&
                ((float)winAC >= (float)winDC * HEART_AC_MIN_REL);

    // ---- ① Maxim 官方算法 ----
    int32_t spo2 = 0, hr = 0;
    int8_t  spOk = 0, hrOk = 0;
    maxim_heart_rate_and_oxygen_saturation(irBuf, HEART_WIN, redBuf,
                                           &spo2, &spOk, &hr, &hrOk);

    bool hrGood = acOk && hrOk && hr   >= HEART_MIN_HR   && hr   <= HEART_MAX_HR;
    bool spGood = acOk && spOk && spo2 >= HEART_MIN_SPO2 && spo2 <= HEART_MAX_SPO2;

    // ---- ② 官方算法否决时用自研算法兜底 ----
    if (!hrGood) {
        int32_t h = 0;
        if (simpleHR(irBuf, &h)) { hr = h; hrGood = true; }
    }
    if (!spGood) {
        int32_t s = 0;
        if (simpleSpO2(irBuf, redBuf, &s)) { spo2 = s; spGood = true; }
    }

    // ---- ③ 两路都没救回来 ----
    if (!hrGood && !spGood) {
        okCnt = 0;
        if (++missCnt >= HEART_MISS_TOLERATE) {
            clearOutput();
        }
        state = HEART_ST_MEASURING;
        return;
    }
    missCnt = 0;

    // ---- ④ 中值滤波 ----
    if (hrGood) {
        pushRing(hrRing, hrRingN, hrRingIdx, hr);
        outHR = medianOf(hrRing, hrRingN);
    }
    if (spGood) {
        pushRing(spRing, spRingN, spRingIdx, spo2);
        outSpO2 = medianOf(spRing, spRingN);
    }

    // 连续拿到有效读数才认为稳定，避免刚放上手指时数值乱跳
    if (okCnt < HEART_STABLE_NEED) okCnt++;
    state = (okCnt >= HEART_STABLE_NEED) ? HEART_ST_OK : HEART_ST_MEASURING;

    /* ★ 只有进入「读数正常」才把数值发出去；测量中一律发 0。
     *   这样界面上的数字要么是稳定读数、要么是「--」，
     *   不会出现上一轮的残值挂在那里、看着像“手拿开了数字还在变”。 */
    if (state == HEART_ST_OK) {
        set_var_health_heartbeat(outHR);
        set_var_health_oxygen(outSpO2);
    } else {
        set_var_health_heartbeat(0);
        set_var_health_oxygen(0);
    }
}

/* ============================================================================
 * 四、逐样本处理（手指判定 + 填窗 + 触发计算）
 * ==========================================================================*/
static void feedSample(uint32_t ir, uint32_t red) {
    // 相邻样本完全不同才可能有两路 LED 的独立读数；完全相同说明是重复读数
    if (lastValid && ir == lastIR && red == lastRed) return;
    lastIR = ir; lastRed = red; lastValid = true;

    outIR = (int32_t)ir;

    // IR 滑动平均：对脉搏交流分量不敏感，只反映“有没有东西挡在传感器上”
    irEma = (irEma <= 0.0f) ? (float)ir
                            : (irEma * (1.0f - HEART_EMA_ALPHA) + (float)ir * HEART_EMA_ALPHA);

    if (!fingerOn) {
        float th = enterThreshold();
        if (irEma < th) {
            updateBaseline(irEma);
            if (state != HEART_ST_NO_FINGER) {
                state = HEART_ST_NO_FINGER;
                resetWindow();
                clearOutput();
            }
            return;                              // 没手指：什么都不做
        }
        fingerOn      = true;                    // 手指刚放上 → 从零攒窗口
        fingerSinceMs = millis();
        resetWindow();
    } else if (irEma < exitThreshold()) {
        fingerOn = false;                        // 手指离开
        resetWindow();
        state = HEART_ST_NO_FINGER;
        clearOutput();
        return;
    }

    // ---- 填窗 ----
    irBuf[samplesIn]  = ir;
    redBuf[samplesIn] = red;
    samplesIn++;

    if (samplesIn < HEART_WIN) {
        state = HEART_ST_MEASURING;              // 还差几个样本，先让网页显示“测量中”
        return;
    }

    // ---- 窗口满：算一次，然后把窗口向前滑 1 秒 ----
    computeOnce();

    /* computeOnce() 里如果判定手指已离开，会调 resetWindow() 把窗口清零
     * （samplesIn = 0）并置 fingerOn = false。此时不能再按“窗口满”去滑动，
     * 否则会把已清空的窗口又写成一个半满的陈旧状态。 */
    if (!fingerOn) return;

    memmove(irBuf,  irBuf  + HEART_STEP, (HEART_WIN - HEART_STEP) * sizeof(uint32_t));
    memmove(redBuf, redBuf + HEART_STEP, (HEART_WIN - HEART_STEP) * sizeof(uint32_t));
    samplesIn = HEART_WIN - HEART_STEP;
}

/* 前置声明：setLedAmp() 会用到（定义在第六节，为了把「取样本」的纪律集中在一处） */
static inline void drainInternal(void);
static uint8_t     fetchSamples(uint32_t *irs, uint32_t *reds, uint8_t maxN);

/* ============================================================================
 * 五、LED 开关
 * ----------------------------------------------------------------------------
 * 关灯时把两路 LED 的驱动电流写成 0，芯片其余部分照常工作；
 * 开灯时先清一次硬件 FIFO —— 否则熄灯期间 FIFO 里积的都是“黑样本”（IR≈0），
 * 一开灯就会被读进采集窗口，把手指判定带偏。
 * ==========================================================================*/
static void setLedAmp(bool on) {
    if (on == ledOn) return;

    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    if (on) {
        sensor.clearFIFO();                       // 丢掉熄灯期间积累的黑样本
        drainInternal();                          // 同步库内部 head/tail
        sensor.setPulseAmplitudeRed(HEART_LED_BRIGHTNESS);
        sensor.setPulseAmplitudeIR(HEART_LED_BRIGHTNESS);
        ledWarmupLeft = HEART_LED_WARMUP_SAMPLES; // 前几个样本作废
    } else {
        sensor.setPulseAmplitudeRed(0);
        sensor.setPulseAmplitudeIR(0);
        sensor.clearFIFO();
        drainInternal();
    }

    xSemaphoreGive(i2c_mutex);
    ledOn = on;
}

/* ============================================================================
 * 六、取样本  ★ 全工程最关键的 I2C 纪律，本轮修复的核心
 * ----------------------------------------------------------------------------
 * 库里内部缓冲只有 4 槽（STORAGE_SIZE = 4），而且**只有 check() 会走 I2C**。
 * 正确姿势：一次 check() 把设备 FIFO 里的新样本全部搬进内部缓冲，然后用
 * getFIFOIR() / getFIFORed()（纯内存读、零 I2C）+ nextSample()（只推 tail）
 * 按时间顺序取走。取到的是互不相同、且 IR/Red 严格配对的样本。
 *
 * ★★ 绝对禁止调用 getIR() / getRed()！库里的实现是：
 *
 *      uint32_t MAX30105::getIR(void) {
 *          if (safeCheck(250)) return sense.IR[sense.head];
 *          else return 0;
 *      }
 *      bool MAX30105::safeCheck(uint8_t maxTimeToCheck) {
 *          while (1) {
 *              if (millis() - markTime > maxTimeToCheck) return false;
 *              if (check() == true) return true;   // ← 判的是「设备里有没有新样本」
 *              delay(1);
 *          }
 *      }
 *
 *   check() 在设备 FIFO 已被读空时返回 0，于是 safeCheck 会 delay(1) 空转，
 *   **一直等下一个样本到来**（25 Hz 下约 40 ms，上限 250 ms）才肯返回。
 *   而这段等待正发生在 i2c_mutex 里面！getIR() + getRed() 各来一次，
 *   单次调用就能独占总线几十到几百毫秒。
 *
 *   后果（就是「一测血氧，温湿度/气压就罢工」的根因）：
 *     · SHT30  取锁超时 100 ms → 拿不到锁 → 读失败 → 界面甩出 -999；
 *     · BMP280 取锁超时 100 ms → 拿不到锁 → 读回 0 → 越界丢弃 → 气压/海拔冻住；
 *     · GY302 / MPU6050 / 触摸控制器 同样被不同程度地饿死。
 *   而手指放在传感器上时 LED 常亮、采集循环从不休眠，总线被独占的比例接近 100%，
 *   于是就成了「不测血氧时勉强正常、一开始测就全挂」。
 *
 *   顺带一提，旧写法「取 head、推 tail」还会把一个样本重复读进窗口 k 次，
 *   让 Maxim 算法归一化除零 —— 现在改用 tail 侧取值，这个问题一并消失。
 * ==========================================================================*/
static uint8_t fetchSamples(uint32_t *irs, uint32_t *reds, uint8_t maxN) {
    sensor.check();                        // 唯一一次 I2C：读指针 + 突发读 FIFO
    uint8_t n = 0;
    while (sensor.available() > 0 && n < maxN) {
        irs[n]  = sensor.getFIFOIR();      // 纯内存读，无 I2C、无阻塞
        reds[n] = sensor.getFIFORed();     // 与上一行取自同一个样本
        sensor.nextSample();               // tail++，与 getFIFO* 严格配对
        n++;
    }
    return n;
}

/* clearFIFO() 只清设备侧的指针，库内部的 head/tail 不会重置 ——
 * 清完必须把内部缓冲同步空，否则 available() 会把清空前的旧值当成新样本取出来。 */
static inline void drainInternal(void) {
    while (sensor.available() > 0) sensor.nextSample();
}

/* 采集一轮：取锁时间被压缩到「一次 check()」，约 1 ms 以内（原来可达数百 ms） */
static uint8_t pollSensor(uint32_t *irs, uint32_t *reds, uint8_t maxN) {
    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
    uint8_t n = fetchSamples(irs, reds, maxN);
    xSemaphoreGive(i2c_mutex);
    return n;
}

/* ============================ 七、初始化 ============================ */
void Heart_Init(void) {
    if (!sensor.begin(Wire, I2C_SPEED_FAST)) {
        Serial.println("[Heart] MAX30105 未找到，请检查接线/供电");
        hOnline = false;
        state   = HEART_ST_OFFLINE;
        return;
    }

    sensor.setup(HEART_LED_BRIGHTNESS, HEART_SAMPLE_AVG, HEART_LED_MODE,
                 HEART_SAMPLE_RATE, HEART_PULSE_WIDTH, HEART_ADC_RANGE);

    // 只保留 Red / IR 两路发光，Green 关闭以省电并降低自热漂移
    sensor.setPulseAmplitudeRed(HEART_LED_BRIGHTNESS);
    sensor.setPulseAmplitudeIR(HEART_LED_BRIGHTNESS);
    sensor.setPulseAmplitudeGreen(0);

    resetWindow();
    clearOutput();
    irEma         = 0.0f;
    irBase        = (float)HEART_IR_ENTER_ABS;   // 先给个保守初值
    fingerOn      = false;
    starve        = 0;
    winDC         = 0;
    winAC         = 0;
    outIR         = 0;
    lastValid     = false;
    ledWarmupLeft = HEART_LED_WARMUP_SAMPLES;
    ledOn         = true;                        // 下面标定期间保持点灯
    lastFingerMs  = millis();
    fingerSinceMs = millis();
    hOnline       = true;
    state         = HEART_ST_NO_FINGER;

    /* ---- 开机基线标定 ----
     * 连采 12 个样本取中值作为「无手指电平」的初值（中值抗单点干扰）。
     * 标定期间请勿把手指压在传感器上，否则基线会被抬高，需要拿开手指
     * 静置约 2 秒让基线自己跟踪回来（正常使用下这是自动的）。 */
    sensor.clearFIFO();
    drainInternal();
    uint32_t cal[12];
    uint8_t  calN = 0;
    uint32_t t0   = millis();
    while (calN < 12 && (millis() - t0) < 1200) {
        uint32_t irs [HEART_BATCH_MAX];
        uint32_t reds[HEART_BATCH_MAX];
        uint8_t  n = 0;
        if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            n = fetchSamples(irs, reds, HEART_BATCH_MAX);
            xSemaphoreGive(i2c_mutex);
        }
        for (uint8_t i = 0; i < n && calN < 12; i++) cal[calN++] = irs[i];
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (calN > 0) {
        for (uint8_t i = 1; i < calN; i++) {      // 插入排序取中值
            uint32_t v = cal[i];
            int8_t   j = (int8_t)i - 1;
            while (j >= 0 && cal[j] > v) { cal[j + 1] = cal[j]; j--; }
            cal[j + 1] = v;
        }
        float base = (float)cal[calN / 2];
        if (base < HEART_BASE_MIN) base = HEART_BASE_MIN;
        if (base > HEART_BASE_MAX) base = HEART_BASE_MAX;
        irBase = base;
    }

    Serial.printf("[Heart] MAX30105 就绪：有效采样 %d Hz，窗口 %d 点 / %d 秒\n",
                  HEART_EFFECTIVE_HZ, HEART_WIN, HEART_WIN / HEART_EFFECTIVE_HZ);
    Serial.printf("[Heart] 无手指基线 IR = %.0f，进入门限 = %.0f，退出门限 = %.0f\n",
                  irBase, enterThreshold(), exitThreshold());
    Serial.println("[Heart] 提示：标定/使用时请把手指轻放在背面传感器上，别用力压。");
}

/* ============================ 八、任务 ============================ */
void Task_Heart(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(500));
    lastFingerMs  = millis();
    fingerSinceMs = millis();

    uint32_t lastDbgMs = millis();

    for (;;) {
        // 名义周期 20 ms（25 Hz 采样下每轮约 0.5 个样本）。
        // 熄灯探测期同样走 20 ms 分支，体检表里周期应当很平稳。
        TaskMon_Tick(TM_HEART, HEART_POLL_MS);

        if (!hOnline) {
            state = HEART_ST_OFFLINE;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // ---- LED 占空比调度 ----
        uint32_t now = millis();
        if (fingerOn) {
            lastFingerMs = now;

            /* 误判自愈：判定「有手指」持续很久却始终得不到有效读数，
             * 多半是背壳串扰/环境光把电平顶上去了。退回无手指状态，
             * 让基线重新学习当前电平。 */
            if (state != HEART_ST_OK && (now - fingerSinceMs) > HEART_FALSE_POS_MS) {
                fingerOn      = false;
                lastFingerMs  = now;
                irEma         = 0.0f;
                resetWindow();
                state = HEART_ST_NO_FINGER;
                clearOutput();
                Serial.println("[Heart] 长时间无有效读数，判定为误检，重新学习基线");
            } else {
                setLedAmp(true);                          // 测量中：LED 常亮
            }
        } else if (now - lastFingerMs > HEART_LED_IDLE_MS) {
            // 长时间没手指 → 只在探测窗口点灯，其余时间完全熄灭
            uint32_t phase = (now - lastFingerMs) % HEART_LED_PROBE_PERIOD;
            setLedAmp(phase < HEART_LED_PROBE_ON);
        } else {
            setLedAmp(true);                              // 刚离开不久：保持常亮，别频繁开关
        }

        if (ledOn) {
            uint32_t irs [HEART_BATCH_MAX];
            uint32_t reds[HEART_BATCH_MAX];
            uint8_t  n = pollSensor(irs, reds, HEART_BATCH_MAX);

            // 刚点灯的几个样本受 LED 电流突变影响，直接跳过
            uint8_t start = 0;
            if (ledWarmupLeft > 0) {
                uint8_t skip = (ledWarmupLeft < n) ? ledWarmupLeft : n;
                ledWarmupLeft = (uint8_t)(ledWarmupLeft - skip);
                start = skip;
            }
            for (uint8_t i = start; i < n; i++) feedSample(irs[i], reds[i]);

            // 看门狗：连续 3 秒一个样本都没有 → 判定异常，退回“未检测到手指”
            if (n == 0) {
                if (starve < 0xFFFF) starve++;
                if (starve >= HEART_STARVE_LIMIT) {
                    starve   = 0;
                    fingerOn = false;
                    resetWindow();
                    if (state != HEART_ST_NO_FINGER) {
                        state = HEART_ST_NO_FINGER;
                        clearOutput();
                    }
                }
            } else {
                starve = 0;
            }

            vTaskDelay(pdMS_TO_TICKS(HEART_POLL_MS));
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));                // 熄灯期：低频挂起
        }

        // ---- 串口诊断（每 2 秒一行）----
        now = millis();
        if (now - lastDbgMs >= 2000) {
            lastDbgMs = now;
            Serial.printf("[Heart] st=%u finger=%d IR=%ld base=%.0f th=%.0f/%.0f "
                          "DC=%ld AC=%ld HR=%ld SpO2=%ld\n",
                          state, fingerOn ? 1 : 0, (long)outIR,
                          irBase, enterThreshold(), exitThreshold(),
                          (long)winDC, (long)winAC, (long)outHR, (long)outSpO2);
        }
    }
}

/* ============================ 九、对外接口 ============================ */
int32_t     Heart_GetHR(void)    { return outHR; }
int32_t     Heart_GetSpO2(void)  { return outSpO2; }
int32_t     Heart_GetIR(void)    { return outIR; }
int32_t     Heart_GetDC(void)    { return winDC; }
int32_t     Heart_GetAC(void)    { return winAC; }
int32_t     Heart_GetBase(void)  { return (int32_t)irBase; }
int32_t     Heart_GetEnterTh(void){ return (int32_t)enterThreshold(); }
uint8_t     Heart_GetState(void) { return state; }
bool        Heart_IsOnline(void) { return hOnline; }

const char *Heart_GetStateText(void) {
    switch (state) {
        case HEART_ST_NO_FINGER: return "未检测到手指";
        case HEART_ST_MEASURING: return "测量中";
        case HEART_ST_OK:        return "读数正常";
        default:                 return "传感器未就绪";
    }
}
