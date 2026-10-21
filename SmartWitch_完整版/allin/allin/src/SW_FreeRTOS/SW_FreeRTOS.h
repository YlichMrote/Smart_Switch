#ifndef _SW_FREERTOS_H
#define _SW_FREERTOS_H

#include "Arduino.h"
#include "../bmp280/bmp280.h"
#include "../gy302/gy302.h"
#include "../heart/heart.h"
#include "../mpu6050/mpu6050.h"
#include "../sht30/sht30.h"
#include "../KeyControl/Key.h"
#include "../prac1/vars.h"
#include "../prac1/parc.h"
#include "../inmp441/inmp441.h"
#include "../Audio/Audio.h"
#include "../BLE/ble_uart.h"
#include "../task_mon/task_mon.h"
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <Wire.h>


//气压 光强  温湿度
#define Time 800
//姿态 
#define HTime 20
// 心率/血氧的采集节拍由 heart.cpp 内部按 25 Hz 有效采样率自行管理
// //LCD刷新时间
// #define RefreshTime 5000
//语音识别的时间间隔
#define LGRefreshTime 300
// 语音任务的优先级：它整段循环都在做阻塞式 I2S 录音 / HTTP 请求，
// 拿不到 CPU 也只是晚几十毫秒，不需要和 UI 抢实时性。
// 原来跟 Task_Parc 同为 2，会和 LVGL 刷新抢时间片导致掉帧，故降到 1。
#define TASK_PRIORITY_VOICE 1
// extern SemaphoreHandle_t xCmdSem;       // 命令信号量
// extern SemaphoreHandle_t xLCDMutex;     // LCD互斥量

extern SemaphoreHandle_t i2c_mutex;

#define I2C_SLOW_SDA  10   // 低速总线（SHT30/BMP280/BH1750）
#define I2C_SLOW_SCL  11


static void scanI2C();
void FreeRTOS_Start(void);
void Multitask_Init(void);

#endif