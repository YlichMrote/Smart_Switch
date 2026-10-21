#ifndef _HEART_H
#define _HEART_H

#include "Arduino.h"

/* ============================================================================
 * MAX30105 心率 / 血氧模块
 * ----------------------------------------------------------------------------
 * 采样链路：LedSampleRate(100 Hz) ÷ FIFO SampleAverage(4) = 25 Hz 有效采样率。
 * 这个 25 Hz 必须与 Maxim 官方算法 spo2_algorithm.h 里的 FreqS 完全一致，
 * 否则心率公式 (FreqS*60)/峰间距 会成比例算错。
 * 窗口 100 点 = 4 秒，可覆盖 3~4 个心动周期，血氧 ratio 样本才够稳定。
 *
 * ★ 取样本必须遵守 SparkFun 库的语义（重要）
 *   MAX30105 库内部只有 4 个样本的环形缓冲（STORAGE_SIZE = 4）。
 *   · sense.head = 最新样本的下标（check() 读硬件 FIFO 时每次 +1）
 *   · sense.tail = 已消费位置，nextSample() 才推进它
 *   · getIR()/getRed() 返回 [head]（最新），getFIFOIR()/getFIFORed() 返回 [tail]
 *   如果 check() 一次补进多个样本，而循环反复从同一个位置取值，就会把同一个
 *   样本重复塞进窗口；窗口里出现成片相同值后，Maxim 算法的归一化步骤
 *   (un_max - un_min)/255 分母为 0，心率血氧会永远出不来。
 *   本模块的做法：每次 check() 之后**只取最新那一个样本**，多出来的追赶样本
 *   直接 nextSample() 跳过，并额外加「相邻样本完全相同就丢弃」的兜底。
 *
 * ★ 手指在位判定（本次重写）
 *   不再用「固定 IR 阈值」，而是用**自适应基线 + 迟滞**：
 *     enter = max(绝对下限, 基线 × 1.8)
 *     exit  = max(绝对下限, 基线 × 1.25)
 *   基线在「判定为没有手指」时以很慢的速度跟踪当前 IR，所以手表背壳/盖板
 *   自身的漏光（串扰）会被自动吸收进去，不会出现「没放手也一直认为有手指」
 *   或「手放上去反而测不到」这两种误判。
 * ==========================================================================*/

// 运行状态（供 UI / BLE 网页显示）
#define HEART_ST_OFFLINE    0   // 传感器未初始化成功（没插/接线不对）
#define HEART_ST_NO_FINGER  1   // 未检测到手指
#define HEART_ST_MEASURING  2   // 检测到手指，读数正在收敛
#define HEART_ST_OK         3   // 读数有效

// 传感器初始化（放在 Multitask_Init 里调用）
void Heart_Init(void);

// 采集 + 计算任务（挂 Core0）
void Task_Heart(void *arg);

// ---- 线程安全的数据快照 ----
int32_t     Heart_GetHR(void);        // 平滑后心率 bpm，无效为 0
int32_t     Heart_GetSpO2(void);      // 平滑后血氧 %，无效为 0
int32_t     Heart_GetIR(void);        // 最近一次 IR 原始采样值
int32_t     Heart_GetDC(void);        // 窗口 IR 直流均值（手指贴合度）
int32_t     Heart_GetAC(void);        // 窗口 IR 峰峰值（脉动幅度 = 信号质量）
int32_t     Heart_GetBase(void);      // 当前自适应 IR 基线（无手指时的电平）
int32_t     Heart_GetEnterTh(void);   // 当前「判定有手指」的门限，便于现场排障
uint8_t     Heart_GetState(void);     // HEART_ST_*
const char *Heart_GetStateText(void); // 状态中文描述
bool        Heart_IsOnline(void);     // 传感器是否就绪

#endif
