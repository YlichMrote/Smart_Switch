#include "parc.h"
#include "screens.h"
#include "../task_mon/task_mon.h"

// -1 表示没有待处理的切屏请求
static volatile int16_t g_reqScreen = -1;

void Parc_RequestScreen(int16_t screenId) {
    g_reqScreen = screenId;
}


// void Screen_On(void);
// void Screen_Off(void);
// void CST820_SetAutoSleep(uint8_t sec);
// void CST820_DisableAutoSleep(bool disable);
// void enterSleepMode();
// void exitSleepMode();
// void IRAM_ATTR touchWakeupCallback();


void Parc_Init(void)
{
    //Serial.begin( 115200 ); /* prepare for possible serial debug */

    /* 1) 先点亮屏幕：初始化 TFT / 触摸 / LVGL 并加载首屏。
     *    这一步必须排在联网之前 —— 否则一旦 Wi-Fi 连不上，
     *    屏幕会一直停在未初始化的黑屏状态，看起来像死机。 */
    screen_init();
    ui_init();

    /* 2) 再并发联网：clock_init() 内部只调 WiFi.begin() 就立即返回，
     *    等待连接 / NTP 对时 / 拉取天气全部交给 Task_WiFi（core0）后台跑，
     *    Task_Parc（core1）不受阻塞，全程按 50ms 节拍正常刷新 UI。 */
    clock_init();

    // pinMode(LCD_BL_PIN, OUTPUT);
    // digitalWrite(LCD_BL_PIN, HIGH);
    // pinMode(CST820_INT_PIN, INPUT_PULLUP);

    // lastTouchTime = millis();
    // CST820_DisableAutoSleep(true);
    // //注册中断唤醒函数
    // attachInterrupt(
    //     digitalPinToInterrupt(CST820_INT_PIN),
    //     touchWakeupCallback,
    //     FALLING
    // );

    Serial.println( "Setup done" );
    
}

void Task_Parc(void* arg)
{
    while(1)
    {
        // 名义周期 UITime=50 ms。实测均值 − 50 就是 LVGL 每帧真正吃掉的
        // CPU 时间（含 lv_timer_handler 渲染 + EEZ flow tick）。
        TaskMon_Tick(TM_PARC, UITime);

        clock_update();

        // 处理来自 BLE 等其它任务的切屏请求（在 UI 任务里执行，LVGL 才安全）
        int16_t req = g_reqScreen;
        if (req >= _SCREEN_ID_FIRST && req <= _SCREEN_ID_LAST) {
            g_reqScreen = -1;
            eez_flow_set_screen(req, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0);
        }

        ui_tick();

        lv_timer_handler(); /* let the GUI do its work */
        //if (!systemSleep) 0.
        //{
            // bool touched = (digitalRead(CST820_INT_PIN) == LOW);
            // if (touched) 
            // {
            //     lastTouchTime = millis();
            // }
            // if (millis() - lastTouchTime > SLEEP_TIMEOUT) 
            // {
            //     enterSleepMode();
            // }
        //}
        //Serial.println("ui任务在运行！！");
        vTaskDelay(pdMS_TO_TICKS(UITime));
    }
    
}

// void Screen_On(void)
// {
//     digitalWrite(LCD_BL_PIN, HIGH);
// }

// void Screen_Off(void)
// {
//     digitalWrite(LCD_BL_PIN, LOW);
// }

// // 设置自动休眠时间
// void CST820_SetAutoSleep(uint8_t sec) 
// {
//   Wire.beginTransmission(CST820_ADDR);
//   Wire.write(CST820_REG_AUTOSLEEPTIME);
//   Wire.write(sec);
//   Wire.endTransmission();
// }

// // 允许 / 禁止自动休眠（软开关，核心！）
// void CST820_DisableAutoSleep(bool disable) 
// {
//   Wire.beginTransmission(CST820_ADDR);
//   Wire.write(CST820_REG_DISAUTOSLEEP);
//   Wire.write(disable ? 0x01 : 0x00);
//   Wire.endTransmission();
// }

// // ==================== 进入低功耗（纯软） ====================
// void enterSleepMode() 
// {
//   systemSleep = true;
//   Screen_Off();           // 熄屏
//   CST820_SetAutoSleep(1); // 1秒内进入休眠
//   CST820_DisableAutoSleep(false); // 允许休眠
// }

// // ==================== 唤醒（纯软！无硬件复位！） ====================
// void exitSleepMode() 
// {
//   systemSleep = false;
//   CST820_DisableAutoSleep(true); // 禁止自动休眠 → 自动唤醒！
//   Screen_On();                    // 亮屏
//   lastTouchTime = millis();
// }

// // ==================== 中断回调函数（你要的！） ====================
// void IRAM_ATTR touchWakeupCallback() 
// {
//   if (systemSleep) {
//     exitSleepMode();
//   }
// }





