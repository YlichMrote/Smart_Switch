#ifndef _PARC_H
#define _PARC_H

#include "Arduino.h"

#include <lvgl.h>
#include "ui.h"
#include "vars.h"
#include "init.h"
#include "clock.h"

#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <Wire.h>

#define UITime  50

// #define LCD_BL_PIN        19
// #define CST820_INT_PIN    40  // 仅用中断，不用RST

// #define CST820_ADDR              0x15
// #define CST820_REG_AUTOSLEEPTIME 0xF9
// #define CST820_REG_DISAUTOSLEEP  0xFE

// bool systemSleep = false;
// unsigned long lastTouchTime = 0;
// const unsigned long SLEEP_TIMEOUT = 10000;  // 10秒

extern uint16_t touch_x;
extern uint16_t touch_y;

void Parc_Init(void);
void Task_Parc(void* arg);

// 请求切换界面：只登记请求，真正的切屏在 Task_Parc 里执行，
// 避免从 BLE / 按键等其它任务直接操作 LVGL 造成线程竞争。
void Parc_RequestScreen(int16_t screenId);




#endif