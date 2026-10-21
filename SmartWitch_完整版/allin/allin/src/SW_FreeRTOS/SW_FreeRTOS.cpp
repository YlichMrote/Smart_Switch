#include "SW_FreeRTOS.h"


SemaphoreHandle_t i2c_mutex = NULL;
SemaphoreHandle_t xCmdSem = NULL;
SemaphoreHandle_t xLCDMutex = NULL;

#define TASK_STACK_SIZE 1024
#define TASK_PRIORITY_SENSOR 1  
#define TASK_PRIORITY_LCD 2     
#define TASK_PRIORITY_HIGH 2   
#define TASK_CORE1 1
#define TASK_CORE0 0 


/* ----------------------------------------------------------------------------
 * MPU6050 参数配置
 * 原代码把这段配置原样写了两遍（第二遍还执行在 begin() 失败的分支外），
 * 这里收敛成一个函数，只在初始化成功后调用一次。
 * --------------------------------------------------------------------------*/
static void MPU6050_Config(void)
{
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    Serial.print("Accel range set to: ");
    switch (mpu.getAccelerometerRange())
    {
        case MPU6050_RANGE_2_G:  Serial.println("+-2G");  break;
        case MPU6050_RANGE_4_G:  Serial.println("+-4G");  break;
        case MPU6050_RANGE_8_G:  Serial.println("+-8G");  break;
        case MPU6050_RANGE_16_G: Serial.println("+-16G"); break;
    }

    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    Serial.print("Gyro range set to: ");
    switch (mpu.getGyroRange())
    {
        case MPU6050_RANGE_250_DEG:  Serial.println("+- 250 deg/s"); break;
        case MPU6050_RANGE_500_DEG:  Serial.println("+- 500 deg/s"); break;
        case MPU6050_RANGE_1000_DEG: Serial.println("+- 1000 deg/s"); break;
        case MPU6050_RANGE_2000_DEG: Serial.println("+- 2000 deg/s"); break;
    }

    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
    Serial.print("Filter bandwidth set to: ");
    switch (mpu.getFilterBandwidth())
    {
        case MPU6050_BAND_260_HZ: Serial.println("260 Hz"); break;
        case MPU6050_BAND_184_HZ: Serial.println("184 Hz"); break;
        case MPU6050_BAND_94_HZ:  Serial.println("94 Hz");  break;
        case MPU6050_BAND_44_HZ:  Serial.println("44 Hz");  break;
        case MPU6050_BAND_21_HZ:  Serial.println("21 Hz");  break;
        case MPU6050_BAND_10_HZ:  Serial.println("10 Hz");  break;
        case MPU6050_BAND_5_HZ:   Serial.println("5 Hz");   break;
    }
}


/* ----------------------------------------------------------------------------
 * 各传感器初始化
 * --------------------------------------------------------------------------*/
static void Sensor_Init(void)
{
    sht3xd.begin(0x44);                     // SHT30 温湿度
    Serial.print("SHT30 Serial #");
    Serial.println(sht3xd.readSerialNumber());

    if (!bmp.begin(0x76))                   // BMP280 气压
    {
        Serial.println("Could not find a valid BMP280 sensor, check wiring!");
    } else {
        /* 打开硬件过采样 + IIR 滤波。默认是 X16 气压过采样 + FILTER_OFF，
         * 气压原始噪声 ±2~3 Pa（≈±20 m 海拔）在界面上肉眼可见地跳；
         * FILTER_X16 后输出非常稳，代价是响应变慢（对流天气无关紧要）。
         * STANDBY_MS_125：连续模式下每 125 ms 采一次，配合 800 ms 轮询完全够。 */
        bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,     // 连续测量
                        Adafruit_BMP280::SAMPLING_X2,     // 温度过采样 ×2
                        Adafruit_BMP280::SAMPLING_X16,    // 气压过采样 ×16
                        Adafruit_BMP280::FILTER_X16,      // IIR 滤波系数 16（最强）
                        Adafruit_BMP280::STANDBY_MS_125); // 采样间隔 125 ms
        Serial.println("BMP280 Init Success! (X16 + IIR x16)");
    }

    /* ★ 必须用「连续测量模式」而不是「单次测量模式」。
     * 单次模式(ONE_TIME_HIGH_RES_MODE)测完一次就进入掉电状态，
     * BH1750 库不会自动重新触发，必须由用户每次都调 configure() —— 原代码没调，
     * 所以数据寄存器里一直是最初那一次的旧值，串口/网页上看到的就是「光照不更新」。
     * 连续模式由传感器自己按 ~1 Hz 循环测量，读出来永远是新的。 */
    lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
    Serial.println("BH1750 Init Success! (continuous high-res mode)");

    Heart_Init();                           // MAX30105 心率/血氧

    if (!mpu.begin(0x68))                   // MPU6050 姿态/计步
    {
        Serial.println("Failed to find MPU6050 chip");
    } else {
        MPU6050_Config();
        Serial.println("MPU6050 Init Success!");
    }
}


void Multitask_Init(void)
{
  Wire.begin(10, 11);

  Serial.begin(115200);
  delay(1000);

  i2c_mutex = xSemaphoreCreateMutex();
  if (i2c_mutex == NULL) {
      Serial.println("Failed to create I2C mutex!");
      while (1) delay(10);
  }

  Sensor_Init();

  startTime = millis();
  initBuffer();

  KeyControl_Init();
  Parc_Init();

  INMP441_Init();

  Audio_Init();

  // 蓝牙数据通道（放最后初始化，此时 WiFi / 音频等都已就绪，内存占用更可控）
  BLE_Init();

  scanI2C();
}

void FreeRTOS_Start(void)
{
    //创建WiFi/网络任务（后台并发连接与管理，UI 在 Task_Parc 里已先亮屏）
    xTaskCreatePinnedToCore(Task_WiFi, "Task_WiFi", TASK_STACK_SIZE * 8, NULL, TASK_PRIORITY_SENSOR, NULL, TASK_CORE0);
    //创建温湿度任务
    xTaskCreatePinnedToCore(Task_SHT30, "Task_SHT30", TASK_STACK_SIZE * 3, NULL, TASK_PRIORITY_SENSOR, NULL, TASK_CORE0);
    //创建大气压任务
    xTaskCreatePinnedToCore(Task_BMP280, "Task_BMP280", TASK_STACK_SIZE * 4, NULL, TASK_PRIORITY_SENSOR, NULL, TASK_CORE0);
    //创建光照强度任务
    xTaskCreatePinnedToCore(Task_GY302, "Task_GY302", TASK_STACK_SIZE * 3, NULL, TASK_PRIORITY_SENSOR, NULL, TASK_CORE0);
    //创建血氧监测任务
    xTaskCreatePinnedToCore(Task_Heart, "Task_Heart", TASK_STACK_SIZE * 4, NULL, TASK_PRIORITY_SENSOR, NULL, TASK_CORE0); 
    //创建姿态传感器任务
    xTaskCreatePinnedToCore(Task_CMPU6050, "Task_CMPU6050", TASK_STACK_SIZE * 4, NULL, TASK_PRIORITY_SENSOR, NULL, TASK_CORE0);
    //创建LCD刷新任务（根据模式显示数据）
    // xTaskCreatePinnedToCore(lcd_refresh_task, "Task_LCD", TASK_STACK_SIZE, NULL, TASK_PRIORITY_LCD, NULL, TASK_CORE);
    //创建UI界面显示任务
    xTaskCreatePinnedToCore(Task_Parc, "Task_Parc", TASK_STACK_SIZE * 6, NULL, TASK_PRIORITY_HIGH, NULL, TASK_CORE1);
    //创建语音控制任务（解析指令切换显示模式）
    // 优先级降到 1：整段循环都是阻塞式 I2S 录音 / HTTP 请求，不需要实时性，
    // 让出 CPU 给同核的 UI 任务，避免 LVGL 刷新被抢时间片掉帧。
    xTaskCreatePinnedToCore(Task_INMP441, "Task_INMP441", TASK_STACK_SIZE * 6, NULL, TASK_PRIORITY_VOICE, NULL, TASK_CORE1);
    //创建按键控制任务
    xTaskCreatePinnedToCore(Task_KeyControl, "Task_KeyControl", TASK_STACK_SIZE*2 , NULL, TASK_PRIORITY_HIGH  , NULL, TASK_CORE1);
    //创建蓝牙数据通道任务（周期上报全部数据 + 响应网页下发的指令）
    xTaskCreatePinnedToCore(Task_BLE, "Task_BLE", TASK_STACK_SIZE * 4, NULL, TASK_PRIORITY_SENSOR, NULL, TASK_CORE0);
    //Serial.println("所有任务创建完成！");
}

void scanI2C() {
  byte error, address;
  int nDevices = 0;
  Serial.println("Scanning I2C...");
  for(address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();
    if (error == 0) {
      Serial.print("Found at 0x");
      Serial.println(address, HEX);
      nDevices++;
    }
  }
  if (nDevices == 0) Serial.println("No devices found!");
}
