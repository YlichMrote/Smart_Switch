#ifndef _SHT30_H
#define _SHT30_H

#include "Arduino.h"
#include "../SW_FreeRTOS/SW_FreeRTOS.h"
#include <FreeRTOS.h>
#include <task.h>

#include "ClosedCube_SHT31D.h"


// 温湿度缓存
extern float sht30_temp;
extern float sht30_hum;

extern ClosedCube_SHT31D sht3xd;


void Task_SHT30(void *arg);

#endif