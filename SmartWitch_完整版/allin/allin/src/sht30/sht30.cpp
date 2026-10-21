#include "sht30.h"

ClosedCube_SHT31D sht3xd;

// 温湿度
float sht30_temp = 0.0f;
float sht30_hum = 0.0f;

/* ============================================================================
 * SHT3x 直读（两段式，把 50 ms 等待移出互斥区）
 * ----------------------------------------------------------------------------
 * 旧写法调用的是 ClosedCube 库的
 *     sht3xd.readTempAndHumidity(SHT3XD_REPEATABILITY_HIGH, SHT3XD_MODE_POLLING, 50)
 * 它内部无论重复度如何，都是一句写死的 delay(50)（第三个参数 timeout 根本没被
 * 用到）。也就是说：每个周期都抱着 i2c_mutex 干等 50 ms —— 这是全工程最长的一次
 * 总线独占。Core0 上另外几个传感器任务的取锁超时只有 50~100 ms，
 *     · BMP280 / GY302 / MPU6050 会各被顶掉一轮；
 *     · Task_Heart 每 10 ms 就要取一次锁，必然连续失败 2~3 轮，
 *       MAX30105 的采样点因此出现空洞。
 *   （后来查明的真正「总线大户」是 heart.cpp 里的 getIR()/getRed()，
 *    见那儿的第六节注释；本条已随之改成 20 ms 轮询。）
 *
 * 改成标准两段式，功能完全等价、总线独占从 50 ms 降到约 1 ms：
 *   ① 取锁 → 发「单次测量·高重复度」命令 0x2400 → 立即放锁   (<1 ms)
 *   ② 不持锁地等 20 ms（高重复度最大转换时间 15 ms，留足余量）
 *   ③ 取锁 → 读 6 字节并用 CRC-8 校验 → 放锁                 (<1 ms)
 * ==========================================================================*/
#define SHT3X_ADDR           0x44
#define SHT3X_CMD_SINGLE_HI  0x2400    // 单次测量·高重复度·不启用时钟拉伸
#define SHT3X_CONV_MS        20        // 转换等待（不持锁）
#define SHT3X_RETRY          3         // 单轮内重试次数
#define SHT3X_FAIL_TO_REPORT 3         // 连续这么多轮都失败才把 -999 写进界面

/* SHT3x 用 CRC-8：多项式 0x31、初值 0xFF、无反转无异或输出。
 * 自检向量：{0xBE, 0xEF} → 0x92（数据手册样例）。 */
static uint8_t sht3xCrc8(const uint8_t *d, uint8_t n)
{
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

// 读一次温湿度；成功返回 true，失败返回 false（不动缓存值）
static bool sht3xReadOnce(void)
{
    // ---- ① 触发一次测量 ----
    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    Wire.beginTransmission(SHT3X_ADDR);
    Wire.write((uint8_t)(SHT3X_CMD_SINGLE_HI >> 8));
    Wire.write((uint8_t)(SHT3X_CMD_SINGLE_HI & 0xFF));
    bool ok = (Wire.endTransmission() == 0);
    xSemaphoreGive(i2c_mutex);
    if (!ok) return false;

    // ---- ② 等转换完成：★ 这里不持有 i2c_mutex，其它传感器可以正常用总线 ----
    vTaskDelay(pdMS_TO_TICKS(SHT3X_CONV_MS));

    // ---- ③ 读结果：T_MSB T_LSB T_CRC RH_MSB RH_LSB RH_CRC ----
    uint8_t d[6] = {0};
    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    ok = (Wire.requestFrom((uint8_t)SHT3X_ADDR, (uint8_t)6) == 6);
    if (ok) {
        for (uint8_t i = 0; i < 6; i++) d[i] = (uint8_t)Wire.read();
    }
    xSemaphoreGive(i2c_mutex);
    if (!ok) return false;

    if (sht3xCrc8(d, 2) != d[2])     return false;   // 温度 CRC
    if (sht3xCrc8(d + 3, 2) != d[5]) return false;   // 湿度 CRC

    uint16_t rawT  = ((uint16_t)d[0] << 8) | d[1];
    uint16_t rawRH = ((uint16_t)d[3] << 8) | d[4];

    // 数据手册换算：T = -45 + 175·S/65535，RH = 100·S/65535
    sht30_temp = -45.0f + 175.0f * (float)rawT / 65535.0f;
    sht30_hum  = 100.0f * (float)rawRH / 65535.0f;
    if (sht30_hum < 0.0f)   sht30_hum = 0.0f;
    if (sht30_hum > 100.0f) sht30_hum = 100.0f;
    return true;
}

//传感器任务：仅采集数据到缓存，不操作LCD
void Task_SHT30(void *arg)
{
    uint8_t failStreak = 0;

    while (1)
    {
        TaskMon_Tick(TM_SHT30, Time);

        /* 偶发一次读失败（总线竞争、恰好在转换窗口内取锁）不应该把 -999 甩到界面上。
         * 单轮内先重试 3 次；仍然失败就保留上一次的有效值，
         * 只有**连续 3 轮**（约 2.4 秒）都读不到，才认为传感器是真的离线。 */
        bool ok = false;
        for (uint8_t i = 0; i < SHT3X_RETRY && !ok; i++) {
            ok = sht3xReadOnce();
            if (!ok) vTaskDelay(pdMS_TO_TICKS(5));
        }

        if (ok) {
            failStreak = 0;
        } else {
            if (failStreak < 0xFF) failStreak++;
            if (failStreak >= SHT3X_FAIL_TO_REPORT) {
                sht30_hum  = -999;
                sht30_temp = -999;
            }
        }

        set_var_environment_temperature((int32_t)sht30_temp);
        set_var_environmrnt_humidity((int32_t)sht30_hum);

        // 转换等待的 20 ms 已经算在本轮里，这里补足到名义周期 Time
        vTaskDelay(pdMS_TO_TICKS(Time - SHT3X_CONV_MS));
    }
}
