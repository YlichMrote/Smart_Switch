#ifndef _GY302_H
#define _GY302_H

#include "Arduino.h"
#include "../SW_FreeRTOS/SW_FreeRTOS.h"
#include <BH1750.h>
#include <FreeRTOS.h>
#include <task.h>


extern BH1750 lightMeter;

// 光强缓存
extern float gy302_lux;

static void generateLightSuggestion(float lux, char* out_buf, size_t buf_len);
void Task_GY302(void *arg);

#endif