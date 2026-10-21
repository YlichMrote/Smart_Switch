#ifndef _KEY_H
#define _KEY_H

#include <stdint.h>
#include <FreeRTOS.h>
#include <task.h>


uint8_t Get_KeyNum(void);
void KeyControl_Init(void);
void Task_KeyControl(void* arg);


#endif