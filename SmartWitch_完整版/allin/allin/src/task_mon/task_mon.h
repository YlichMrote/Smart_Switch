#ifndef _TASK_MON_H
#define _TASK_MON_H

#include "Arduino.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* ============================================================================
 * 任务体检（Task Monitor）
 * ----------------------------------------------------------------------------
 * 目的：把「每个 FreeRTOS 任务的实际循环周期」和「栈剩余量」量出来，
 *       而不是靠猜。只在任务循环开头调一行 TaskMon_Tick()，开销约 1~2 µs。
 *
 * 统计口径：
 *   · 周期 = 本次进入循环 与 上次进入循环 之间的 tick 差（毫秒）。
 *     名义周期是 vTaskDelay 的设定值，实测周期 = 名义周期 + 本轮实际执行耗时，
 *     所以「实测均值 − 名义周期」就等于这个任务每轮真正吃掉的 CPU 时间。
 *   · 栈余量来自 uxTaskGetStackHighWaterMark()（ESP-IDF 下单位是字节），
 *     每 32 个周期测一次，记录历史最小值。
 *
 * 报告每 10 秒由 Task_BLE 打印一次到串口，同时把「全任务最小栈余量」和
 * 「最大周期超时」两个汇总量通过 BLE 上报到网页。
 * ==========================================================================*/

/* 槽位（顺序与 task_mon.cpp 里的名字表一一对应，不能插队） */
enum {
    TM_SHT30 = 0,
    TM_BMP280,
    TM_GY302,
    TM_HEART,
    TM_MPU6050,
    TM_PARC,
    TM_VOICE,
    TM_KEY,
    TM_WIFI,
    TM_BLE,
    TM_COUNT
};

// 任务循环体开头调用一次。nominalMs 传 0 表示「周期不适用」（如语音任务）
void TaskMon_Tick(uint8_t slot, uint16_t nominalMs);

// 每 10 秒最多打印一次体检表；返回值表示本次是否真的打印了
bool TaskMon_Report(void);

// 全任务最小剩余栈（字节），未测到返回 0
uint16_t TaskMon_MinStackFree(void);

// 本统计窗口内「实际周期 − 名义周期」的最大值（ms）
uint16_t TaskMon_MaxLagMs(void);

#endif
