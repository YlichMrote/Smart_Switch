#include "task_mon.h"

/* ============================================================================
 * 实现说明
 * ----------------------------------------------------------------------------
 * 全部统计量都放在静态数组里，不动态分配内存。
 * 每个槽位只保存「本窗口」的周期样本（打印后清零），而栈最小余量是
 * 从上电以来的历史最小值（永远不清零，因为栈溢出风险只关心最坏情况）。
 * ==========================================================================*/

typedef struct {
    uint16_t nominal;     // 名义周期（ms）
    uint32_t lastTick;    // 上次进入循环时的 tick
    uint16_t n;           // 本窗口周期样本数
    uint32_t sumMs;       // 本窗口周期总和
    uint16_t minMs;       // 本窗口最小周期
    uint16_t maxMs;       // 本窗口最大周期
    uint16_t stackFree;   // 最近一次测得的栈余量
    uint16_t stackMin;    // 历史最小栈余量（上电以来）
} TMonSlot;

static TMonSlot s_slot[TM_COUNT];

static const char* const s_name[TM_COUNT] = {
    "SHT30", "BMP280", "GY302", "Heart", "MPU6050", "Parc", "Voice", "Key", "WiFi", "BLE"
};

static uint32_t s_lastReportMs = 0;

#define TM_REPORT_INTERVAL_MS  10000UL

void TaskMon_Tick(uint8_t slot, uint16_t nominalMs)
{
    if (slot >= TM_COUNT) return;

    TMonSlot* s = &s_slot[slot];
    uint32_t now = xTaskGetTickCount();
    s->nominal = nominalMs;

    if (s->lastTick == 0) {           // 首次进来只记基准点
        s->lastTick = now;
        return;
    }

    uint32_t p = (now - s->lastTick) * (uint32_t)portTICK_PERIOD_MS;
    s->lastTick = now;
    if (p == 0) return;               // 同一 tick 内重入，忽略

    if (s->n == 0) {                  // 新窗口从干净状态开始
        s->sumMs = 0;
        s->minMs = 0xFFFF;
        s->maxMs = 0;
    }
    s->n++;
    s->sumMs += p;
    if (p < s->minMs) s->minMs = (uint16_t)p;
    if (p > s->maxMs) s->maxMs = (uint16_t)p;

    /* uxTaskGetStackHighWaterMark 要扫描整段栈找填充图案，别每圈都调；
     * 每 32 圈测一次足够反映栈用量变化。 */
    if ((s->n & 0x1F) == 0) {
        UBaseType_t w = uxTaskGetStackHighWaterMark(NULL);
        s->stackFree = (uint16_t)w;
        if (s->stackMin == 0 || w < s->stackMin) s->stackMin = (uint16_t)w;
    }
}

bool TaskMon_Report(void)
{
    uint32_t now = millis();
    if (s_lastReportMs != 0 && (now - s_lastReportMs) < TM_REPORT_INTERVAL_MS) return false;
    s_lastReportMs = now;

    Serial.println("[Mon] 任务时序体检：名义 / 实测均值 / 最小 / 最大 (ms)  | 栈余量 B");
    for (uint8_t i = 0; i < TM_COUNT; i++) {
        TMonSlot* s = &s_slot[i];
        if (s->n == 0 && s->stackMin == 0) continue;

        if (s->n == 0) {
            // 周期样本不足（例如语音任务的循环长达数秒），只报栈
            Serial.printf("  %-8s     -  /    -  /    -  /    -     | 余 %5u 最小 %5u\n",
                          s_name[i], s->stackFree, s->stackMin);
        } else {
            uint16_t avg = (uint16_t)(s->sumMs / s->n);
            Serial.printf("  %-8s %5u / %5u / %5u / %5u     | 余 %5u 最小 %5u\n",
                          s_name[i], s->nominal, avg, s->minMs, s->maxMs,
                          s->stackFree, s->stackMin);
        }

        s->n     = 0;      // 周期统计按窗口清零
        s->sumMs = 0;
        s->minMs = 0xFFFF;
        s->maxMs = 0;
        // stackMin 保留：栈最坏情况要看上电以来的最小值
    }
    return true;
}

uint16_t TaskMon_MinStackFree(void)
{
    uint16_t m = 0;
    for (uint8_t i = 0; i < TM_COUNT; i++) {
        uint16_t v = s_slot[i].stackMin;
        if (v == 0) continue;
        if (m == 0 || v < m) m = v;
    }
    return m;
}

uint16_t TaskMon_MaxLagMs(void)
{
    uint16_t m = 0;
    for (uint8_t i = 0; i < TM_COUNT; i++) {
        TMonSlot* s = &s_slot[i];
        if (s->n == 0 || s->nominal == 0) continue;
        if (s->maxMs <= s->nominal) continue;
        uint16_t lag = (uint16_t)(s->maxMs - s->nominal);
        if (lag > m) m = lag;
    }
    return m;
}
