#include "bmp280.h"
#include <math.h>

Adafruit_BMP280 bmp;
// 气压/海拔
float bmp280_press = 0.0f;   // Pa（已做 8 点滑动平均）
float bmp280_alti  = 0.0f;   // m（由平均后的气压换算）

static uint8_t bmpFailStreak = 0;   // 连续读取失败轮数（用于串口提示）

/* ============================================================================
 * BMP280 大气压 / 海拔采集任务
 * ----------------------------------------------------------------------------
 * 一、原来为什么「不准」
 *   1) 传感器没配过采样与 IIR 滤波。Adafruit 库 begin() 里用的是
 *      setSampling(MODE_NORMAL, X1, X16, FILTER_OFF, STANDBY_0.5ms)，
 *      **FILTER_OFF** 时气压原始噪声可达 ±2~3 Pa，换算成海拔就是 ±20~30 m 在跳，
 *      数字看着就是「不准」。
 *   2) 每轮读了两遍。旧代码 `bmp.readPressure()` 之后再调
 *      `bmp.readAltitude(1013.25)`，而 Adafruit 的 readAltitude() 内部**又**会
 *      调一次 readPressure() —— 一次循环白读一遍气压（各含一次温度补偿计算）。
 *   3) 海拔基准 1013.25 hPa 是「标准海平面气压」，而实际海平面气压随天气在
 *      约 980~1040 hPa 之间变。差 7 hPa ≈ 海拔差 58 m，所以绝对海拔必然偏。
 *
 * 二、本次做法
 *   1) Sensor_Init() 里用 setSampling() 打开 X16 气压过采样 + FILTER_X16，
 *      硬件级滤波后输出本身就是平滑的；
 *   2) 每轮只读一次气压，海拔在软件里自己按国际标准大气公式换算；
 *   3) 再做 8 点滑动平均，消掉残余抖动；
 *   4) 基准气压做成可标定的常量 BMP280_SEA_LEVEL_HPA：想知道**真实海拔**，
 *      把它改成当地气象台播报的海平面气压即可（见下方注释）。
 * ==========================================================================*/

/* ----------------------------------------------------------------------------
 * 海平面气压基准（hPa）
 *  · 1013.25 = 国际标准大气，随手可用，但它不是「今天的实际值」，
 *    所以得到的海拔会在 ±60 m 内随天气漂移；
 *  · 要更准：查一下本地气象预报里的「海平面气压 / 场面气压」，填进来。
 *    例如播报 1021 hPa，就写 1021.0f。
 * --------------------------------------------------------------------------*/
#define BMP280_SEA_LEVEL_HPA   1013.25f

#define BMP280_AVG_N           8        // 滑动平均点数
#define BMP280_P_MIN           30000.0f // 合理范围 300 hPa（珠峰级别）
#define BMP280_P_MAX           110000.0f// 合理范围 1100 hPa
#define BMP280_RETRY           3        // 读失败时的重试次数

/* 读一次气压：失败（取锁超时或读数越界）时重试，仍失败返回 false。
 * 单次偶发失败只记录、不参与平均，避免把 0 当成气压算进滑动窗口。 */
static bool bmp280ReadPressure(float *out)
{
    for (uint8_t i = 0; i < BMP280_RETRY; i++)
    {
        if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            float p = bmp.readPressure();      // Pa，函数内部已完成温度补偿
            xSemaphoreGive(i2c_mutex);
            if (p > BMP280_P_MIN && p < BMP280_P_MAX) { *out = p; return true; }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

void Task_BMP280(void *arg)
{
    static float   pBuf[BMP280_AVG_N] = {0};
    static uint8_t pIdx = 0;
    static uint8_t pN   = 0;

    while (1)
    {
        TaskMon_Tick(TM_BMP280, Time);

        float p = 0.0f;
        bool  ok = bmp280ReadPressure(&p);

        /* 读失败时保留上一次的有效值（界面数字不动，而不是跳到 0），
         * 连续失败才在串口提示，便于现场排查接线/总线问题。 */
        if (ok)
        {
            pBuf[pIdx] = p;
            pIdx = (uint8_t)((pIdx + 1) % BMP280_AVG_N);
            if (pN < BMP280_AVG_N) pN++;

            float sum = 0.0f;
            for (uint8_t i = 0; i < pN; i++) sum += pBuf[i];
            bmp280_press = sum / (float)pN;

            /* 国际标准大气公式：h = 44330 × [1 − (p / p0)^0.1903]
             * 与 Adafruit 库内部同一套公式，只是这里用平均后的气压、只读一次。 */
            float hpa = bmp280_press / 100.0f;
            bmp280_alti = 44330.0f *
                          (1.0f - powf(hpa / BMP280_SEA_LEVEL_HPA, 0.1903f));

            // 界面上的海拔是整数变量，四舍五入避免 89.9 显示成 89 的跳变
            set_var_environment_altitude((int32_t)lroundf(bmp280_alti));
            bmpFailStreak = 0;
        }
        else
        {
            if (bmpFailStreak < 0xFF) bmpFailStreak++;
            if (bmpFailStreak == 1 || (bmpFailStreak % 10) == 0)
            {
                Serial.printf("[BMP280] 连续 %u 轮读取失败（I2C 取锁超时或数据越界）\n",
                              (unsigned)bmpFailStreak);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(Time));
    }
}
