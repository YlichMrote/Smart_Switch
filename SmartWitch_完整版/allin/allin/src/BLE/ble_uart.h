#ifndef _BLE_UART_H
#define _BLE_UART_H

#include "Arduino.h"

/*
 * BLE 数据通道（兼容 Nordic UART Service）
 * ------------------------------------------------------------
 *  手机 / 浏览器  ──write──▶  RX 特征值 (6E400002)  →  解析指令
 *  手机 / 浏览器  ◀─notify──  TX 特征值 (6E400003)  ←  周期上报 JSON
 *
 *  上报内容覆盖工程全部功能：环境(温湿度/气压/海拔/光照)、天气出行、
 *  主页(电量/步数/卡路里/日期时间)、健康(心率/血氧)、运动(距离/配速/
 *  时长/卡路里)、系统(当前界面/WiFi/按键/语音状态)。
 *
 *  下发的文本指令：
 *    screen:1..5          切换界面(1环境 2主页 3健康 4运动 5光照)
 *    key:left / key:right 模拟左/右按键翻页
 *    audio:haode          播放语音“好的”
 *    audio:wozai          播放语音“我在”
 *    sync                 立即回传一帧
 */

#define BLE_DEVICE_NAME   "SmartWitch"
#define BLE_SVC_UUID      "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define BLE_RX_UUID       "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"   // 写
#define BLE_TX_UUID       "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"   // 通知

// 周期上报间隔(ms)
#define BLE_REPORT_INTERVAL 500
// 通知分片的字节上限。实际分片大小取 min(协商 MTU − 3, 本值)。
// 桌面浏览器通常协商到 517，取 497 时一帧约 1.3 KB 只需 3 包；
// 原来写死 180 要切 7 包、包间还各留 8 ms，白白多占用 30 ms。
#define BLE_CHUNK_MAX       500

void  BLE_Init(void);
void  Task_BLE(void *arg);

bool  BLE_IsConnected(void);
void  BLE_RequestSync(void);              // 下一轮立刻上报
void  BLE_PushEvent(const String &e);     // 追加一条事件(随下一帧一起发出)

// 内部使用：分块发送一段文本，保证长 JSON 不会被 MTU 截断
bool  BLE_NotifyText(const String &text);

#endif
