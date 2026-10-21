#include "Key.h"
#include "Arduino.h"
#include "../prac1/eez-flow.h"
#include "../prac1/screens.h"
#include "../prac1/parc.h"
#include "../task_mon/task_mon.h"
#include <lvgl.h>

#define Key_LeftButton  41
#define Key_RightButton 42
//定义消除抖动的延时时间
#define ButtonTime 2 
#define ButtonDelayTime 150


void KeyControl_Init(void)
{
    pinMode(Key_LeftButton, INPUT_PULLUP);
    pinMode(Key_RightButton, INPUT_PULLUP);

    digitalWrite(Key_LeftButton, HIGH);
    digitalWrite(Key_RightButton, HIGH);
}


uint8_t Get_KeyNum(void)
{
    uint8_t Key_Num = 0;

    if (digitalRead(Key_LeftButton) == LOW)
    {
        vTaskDelay(pdMS_TO_TICKS(ButtonTime));
        // 等待松手：这里只是"发呆了"，用 10 ms 粒度轮询就够了，
        // 原来 2 ms 一次等于按住期间空转 500 Hz，纯属浪费 CPU
        while (digitalRead(Key_LeftButton) == LOW)
            vTaskDelay(pdMS_TO_TICKS(10));

        Key_Num = 1;
    }

    else if (digitalRead(Key_RightButton) == LOW)
    {
        vTaskDelay(pdMS_TO_TICKS(ButtonTime));
        while (digitalRead(Key_RightButton) == LOW)
            vTaskDelay(pdMS_TO_TICKS(10));

        Key_Num = 2;
    }

    //Serial.print("KeyNum = ");
    //Serial.println(Key_Num);
    return Key_Num;
}

int16_t ScreenIndex;

void Task_KeyControl(void* arg)
{
    uint8_t KeyNum = 0;
    while(1)
    {
        TaskMon_Tick(TM_KEY, ButtonDelayTime);

        KeyNum = Get_KeyNum();
        if(KeyNum != 0)
        {
            ScreenIndex = eez_flow_get_current_screen();
            if(KeyNum == 1)
            {
                ScreenIndex = (ScreenIndex==_SCREEN_ID_FIRST) ?_SCREEN_ID_LAST : ScreenIndex-1;
            }

            else if(KeyNum == 2)
            {
                ScreenIndex = (ScreenIndex==_SCREEN_ID_LAST) ?_SCREEN_ID_FIRST : ScreenIndex+1;
            }

            // ★ 不要在这里直接调 eez_flow_set_screen()：
            //   本任务与 Task_Parc 同核（Core1）同优先级（2），FreeRTOS 会在
            //   每个 tick 做时间片轮转，可能在 LVGL 内部任意位置切走，
            //   造成 LVGL 重入。统一登记请求，由 Task_Parc 在 UI 上下文里执行
            //   （工程约定：LVGL 只允许在 Task_Parc 里操作）。
            if (ScreenIndex >= _SCREEN_ID_FIRST && ScreenIndex <= _SCREEN_ID_LAST)
            {
                Parc_RequestScreen(ScreenIndex);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(ButtonDelayTime));
    }
}
