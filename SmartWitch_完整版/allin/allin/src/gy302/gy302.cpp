#include "gy302.h"

BH1750 lightMeter;

float gy302_lux = 0.0f;// 光强

char light_suggestion[50] = {0};

/* ============================================================================
 * 光照（BH1750 / GY-302）采集任务
 * ----------------------------------------------------------------------------
 * 一、模式问题（本次修复的核心）
 *     旧代码在 init 里用 `lightMeter.begin(BH1750::ONE_TIME_HIGH_RES_MODE)`，
 *     即「单次高分辨率测量」。这种模式下传感器测完一次就自动掉电，数据寄存器
 *     保持那一次的旧值；BH1750 库**不会**自动重新触发，必须每轮手动再调一次
 *     configure()（官方 BH1750onetime 示例最后那行就是干这个的）。旧代码没有这行，
 *     于是 lux 永远停在开机第一次测到的值 → 表现为「光照强度数值不更新」。
 *
 *     现在改成 CONTINUOUS_HIGH_RES_MODE：传感器自己按约 1 Hz 循环测量，
 *     我们只管读最新结果，既简单又不会读到陈旧值。
 *
 * 二、量程
 *     默认 MTreg = 69 时量程 0~54612 lx、分辨率 1.2 lx，足够覆盖
 *     「昏暗 → 室内 → 室外 → 强光」全部提示档位（最高档阈值 10000 lx）。
 *     旧代码那套「按 lux 切 MTreg」的自适应量程有个死循环缺陷：
 *     MTreg=138 时量程上限只有 27306 lx，永远触发不到 >40000 那一档；
 *     而且每次 setMTreg() 内部要发 3 条 I2C 事务 + delay(10)，
 *     等于每 800 ms 白占总线 10 ms。这里直接去掉，改为固定档位。
 *
 * 三、读失败保护
 *     readLightLevel() 读取失败返回 -1、未配置返回 -2，
 *     旧代码会把 -1 直接写进界面。这里只在 lux >= 0 时更新缓存。
 * ==========================================================================*/
void Task_GY302(void *arg)
{
    while (1) {
        TaskMon_Tick(TM_GY302, Time);

        if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            /* 连续模式下 measurementReady() 只要距上次读取超过一次转换时间
             * （高分辨率 + MTreg 69 约 120 ms）就返回 true；本任务周期 800 ms，
             * 所以基本每次都为 true，读到的都是新鲜值。 */
            if (lightMeter.measurementReady(true))
            {
                float lux = lightMeter.readLightLevel();
                if (lux >= 0.0f)                 // -1 / -2 是读取失败或未配置
                {
                    gy302_lux = lux;
                }
            }
            xSemaphoreGive(i2c_mutex);
        }

        generateLightSuggestion(gy302_lux, light_suggestion, sizeof(light_suggestion));
        set_var_lightness(gy302_lux);
        set_var_lightness_text(light_suggestion);

        vTaskDelay(pdMS_TO_TICKS(Time));
    }
}

void generateLightSuggestion(float lux, char* out_buf, size_t buf_len) {
    if (lux <= 50.0) {
        strncpy(out_buf, "环境昏暗，请及时提高亮度", buf_len - 1);
    } else if (lux <= 300.0) {
        strncpy(out_buf, "室内舒适光，适合阅读", buf_len - 1);
    } else if (lux <= 1000.0) {
        strncpy(out_buf, "环境明亮", buf_len - 1);
    } else if (lux <= 10000.0) {
        strncpy(out_buf, "室外自然光，注意防晒", buf_len - 1);
    } else {
        strncpy(out_buf, "强光环境，注意护眼", buf_len - 1);
    }
    out_buf[buf_len - 1] = '\0';
}
