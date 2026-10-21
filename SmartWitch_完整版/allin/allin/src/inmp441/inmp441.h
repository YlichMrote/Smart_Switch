#ifndef _INMP441_H
#define _INMP441_H

#include <WiFi.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include "Arduino.h"
#include "../SW_FreeRTOS/SW_FreeRTOS.h"
#include "../prac1/eez-flow.h"
#include "../prac1/screens.h"
#include "../Audio/Audio.h"
#include <lvgl.h>
#include <FreeRTOS.h>
#include <task.h>
#include <esp_heap_caps.h>

/* INMP441 麦克风引脚定义（按实际接线修改） */
#define I2S_SCK_PIN 6
#define I2S_WS_PIN  7
#define I2S_SD_PIN  15

void INMP441_Init();
void INMP441_NetworkReady();   // 由 Task_WiFi 在联网成功后回调
void Task_INMP441(void* arg);

/* 供网页 / BLE 读取的语音状态：0=待唤醒 1=已唤醒(等指令) 2=识别中 */
uint8_t     INMP441_GetState(void);
const char* INMP441_GetHeard(void);     // 最近一次识别到的文本
const char* INMP441_GetLastCmd(void);   // 最近一次命中的指令文本

#endif
