#ifndef _BMP280_H
#define _BMP280_H

#include "Arduino.h"
#include "../SW_FreeRTOS/SW_FreeRTOS.h"
#include <Adafruit_BMP280.h>

#include <FreeRTOS.h>
#include <task.h>

extern Adafruit_BMP280 bmp;

// 气压/海拔缓存
extern float bmp280_press;
extern float bmp280_alti;

void Task_BMP280(void *arg);


#endif