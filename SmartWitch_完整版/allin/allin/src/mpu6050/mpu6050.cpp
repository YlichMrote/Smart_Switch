#include "mpu6050.h"

Adafruit_MPU6050 mpu;
sensors_event_t a, w, t;

// 姿态
float mpu6050_roll = 0.0f;
float mpu6050_pitch = 0.0f;

// 卡尔曼平滑后的合加速度（mg）。原先这个结果只喂给了一段
// 已经被删除的、输出无人使用的动态阈值计算；现在把它保留为
// 对外可见的平滑值，供后续姿态/计步扩展与调试使用。
float mpu6050_accFilt = 0.0f;

int32_t Data = 0;

// ========== 变量声明 ==========
float Q_angle = 0.001; // 卡尔曼滤波参数：过程噪声协方差
float Q_bias = 0.003;  // 卡尔曼滤波参数：偏置噪声协方差
float R_measure = 0.03; // 卡尔曼滤波参数：测量噪声协方差

float angle = 0.0; // 状态变量：角度
float bias = 0.0;  // 状态变量：偏置
float rate = 0.0;  // 测量变量：角速度
float P[2][2] = {{1, 0}, {0, 1}}; // 协方差矩阵
float K[2]; // 卡尔曼增益
float y, S;

unsigned long lastPeakTime = 0;
unsigned long startTime = 0;
int stepCount = 0;
float totalDistance = 0.0;
unsigned long lastPrintTime = 0;

// 用于计算瞬时配速的环形缓冲区 (存储最近10个步伐的时间间隔)
unsigned long intervalBuffer[INTERVAL_BUFFER_SIZE];
int bufferIndex = 0;
bool bufferFull = false;

// ========== 计步优化相关变量 ==========
#define MIN_STEP_INTERVAL 50    // 最小步间隔(ms)
#define MAX_STEP_INTERVAL 1500  // 最大步间隔(ms)

// 说明：原代码里还有一套「50 点滑窗 → 均值 + k·标准差 → dynamicThreshold」的
// 统计逻辑（WINDOW_SIZE / accWindow / windowIndex / dynamicThreshold / baseAcc /
// accVariance）。它每 20 ms 就跑一次 50 次浮点循环加一次 sqrt，但算出来的
// dynamicThreshold 从头到尾没有被任何地方读取（真正的计步阈值在 detectStep()
// 内部用自己维护的 20 点均值 + 2.0 计算），属于纯粹的 CPU 浪费，已删除。

// 改进的峰值检测状态机
enum StepState {
  STATE_WAITING,      // 等待波峰
  STATE_PEAK_DETECTED,  // 检测到波峰
  STATE_VALLEY_DETECTED // 检测到波谷
};
StepState stepState = STATE_WAITING;

float currentPeak = 0.0;
float currentValley = 0.0;
unsigned long peakTime = 0;
unsigned long valleyTime = 0;

// 传感器校准参数
float sensorOffsetX = 0.0;
float sensorOffsetY = 0.0;
float sensorOffsetZ = 0.0;
bool sensorCalibrated = false;

void Task_CMPU6050(void *arg)
{
  // 初始化传感器校准
  if (!sensorCalibrated) {
    if(xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(200)) == pdTRUE)
    {
      calibrateSensor();
      xSemaphoreGive(i2c_mutex);
    }
    else
    {
      Serial.println("MPU6050 calibration skipped: I2C mutex busy");
    }
  }

  initBuffer();

  uint32_t lastLoopMs = millis();

  while(1)
  {
    TaskMon_Tick(TM_MPU6050, HTime);

    if(xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
      mpu.getEvent(&a, &w, &t);
      xSemaphoreGive(i2c_mutex);
      //Serial.printf("MPU6050: a=%d, b=%d, c=%d\n", a.acceleration.x, a.acceleration.y, a.acceleration.z);
    }else{
      vTaskDelay(pdMS_TO_TICKS(HTime)); 
      continue;
    }
    //float roll = atan2(a.acceleration.y,a.acceleration.z) /3.141592 *180;
    //float pitch = atan2(a.acceleration.x,a.acceleration.z) /3.141592 *180;

    // 1. 计算合加速度T
    float accX = a.acceleration.x;
    float accY = a.acceleration.y;
    float accZ = a.acceleration.z ;
    float accMagnitude = sqrt(accX * accX + accY * accY + accZ * accZ);

    /* dt 必须用「实测间隔」，不能用 HTime 常量：
     * 实际循环时间 = HTime + I2C 读取耗时 + 等锁时间，通常在 21~26 ms 之间波动。
     * 固定按 0.02 算，会让卡尔曼的预测项 (angle += dt*rate) 系统性偏小，
     * 角度/偏置随着时间慢慢跑偏。 */
    uint32_t nowMs = millis();
    float dt = (float)(nowMs - lastLoopMs) / 1000.0f;
    lastLoopMs = nowMs;
    if (dt < 0.001f || dt > 0.5f) dt = (float)HTime / 1000.0f;   // 异常值兜底

    mpu6050_accFilt = KalmanFilter(accMagnitude, 0, dt);

    if (detectStep(accMagnitude)) {
      stepCount++;
      
      // 计算步伐间隔并更新缓冲区
      if (stepCount > 1) {
        unsigned long interval = millis() - lastPeakTime;
        addToBuffer(interval);
      }
      lastPeakTime = millis();
    }

    // 4. 计算距离和配速
    totalDistance = stepCount * STEP_LENGTH_CM / 100.0; // 转换为米

    unsigned long runTime = millis() - startTime;
    float paceMsPerKm = 0.0;

    // 只有在走了几步之后才计算瞬时配速
    unsigned long avgInterval = getAverageInterval();
    if (avgInterval > 0) {
      paceMsPerKm = avgInterval * 1000.0; // 毫秒/公里
    }

    // 5. 每隔1秒打印一次数据
    if (millis() - lastPrintTime > 1000) {
      lastPrintTime = millis();

      // 格式化配速
      int minutes = (int)(paceMsPerKm / 60000);
      int seconds = (int)((paceMsPerKm - minutes * 60000) / 1000);

  
      set_var_main_step(stepCount);
      set_var_sport_distance(totalDistance);
      Data = (int32_t)(totalDistance * 75 / 1000); // 先计算，用足够大的类型存储
  
      set_var_sport_cal(Data);
  

      if (avgInterval == 0) {
        //Serial.print("--分--秒");
      } else {
        if (minutes < 10) Serial.print("0");
  
        if (seconds < 10) Serial.print("0");
    
      }
      set_var_sport_time((int32_t) runTime/60000);
      set_var_sport_pace(int32_t(paceMsPerKm/60000));
      // Serial.println("/公里");
    }
    vTaskDelay(pdMS_TO_TICKS(HTime)); 
  }
}

// 初始化缓冲区
void initBuffer() {
  for (int i = 0; i < INTERVAL_BUFFER_SIZE; i++) {
    intervalBuffer[i] = 0;
  }
}

// 向缓冲区添加新的时间间隔
void addToBuffer(unsigned long interval) {
  intervalBuffer[bufferIndex] = interval;
  bufferIndex = (bufferIndex + 1) % INTERVAL_BUFFER_SIZE;
  if (bufferIndex == 0) bufferFull = true;
}

// 计算缓冲区中所有有效数据的平均值
unsigned long getAverageInterval() {
  unsigned long sum = 0;
  int count = bufferFull ? INTERVAL_BUFFER_SIZE : bufferIndex;
  
  if (count == 0) return 0;

  for (int i = 0; i < count; i++) {
    if (intervalBuffer[i] > 0) {
      sum += intervalBuffer[i];
    }
  }
  return sum / count;
}

// 卡尔曼滤波函数 (一维)
float KalmanFilter(float newAngle, float newRate, float dt) {
  // 预测阶段
  rate = newRate - bias;
  angle += dt * rate;

  P[0][0] += dt * (dt * P[1][1] - P[0][1] - P[1][0] + Q_angle);
  P[0][1] -= dt * P[1][1];
  P[1][0] -= dt * P[1][1];
  P[1][1] += Q_bias * dt;

  // 更新阶段
  y = newAngle - angle;
  S = P[0][0] + R_measure;
  K[0] = P[0][0] / S;
  K[1] = P[1][0] / S;

  angle += K[0] * y;
  bias += K[1] * y;

  P[0][0] -= K[0] * P[0][0];
  P[0][1] -= K[0] * P[0][1];
  P[1][0] -= K[1] * P[0][0];
  P[1][1] -= K[1] * P[0][1];

  return angle;
}

void calibrateSensor() {
  const int calibrationSamples = 100;
  float sumX = 0, sumY = 0, sumZ = 0;

  Serial.println("正在校准传感器...");
  
  for (int i = 0; i < calibrationSamples; i++) {
    mpu.getEvent(&a, &w, &t);
    sumX += a.acceleration.x;
    sumY += a.acceleration.y;
    sumZ += a.acceleration.z;
  }

  // 计算偏移量（静止时应该接近重力加速度）
  sensorOffsetX = sumX / calibrationSamples;
  sensorOffsetY = sumY / calibrationSamples;
  sensorOffsetZ = sumZ / calibrationSamples;

  sensorCalibrated = true;
  Serial.println("传感器校准完成");
}

// ========== 改进的计步检测 ==========
// ========== 使用动态阈值的计步检测 ==========
bool detectStep(float accMagnitude) {
    static float lastAcc = 0.0;
    static bool wasAbove = false;
    static unsigned long lastStepTime = 0;
    static float threshold = 9.8 + 2.0;  // 初始阈值
    
    unsigned long currentTime = millis();
    
    // 防止抖动
    if (currentTime - lastStepTime < MIN_STEP_INTERVAL) {
        lastAcc = accMagnitude;
        return false;
    }
    
    // 更新动态阈值（每 10 次采样更新一次，避免频繁计算）
    static int sampleCount = 0;
    sampleCount++;
    if (sampleCount >= 10) {
        sampleCount = 0;
        // 简单滑动平均：取最近 20 次采样的平均值 + 2.0 偏移
        static float history[20] = {0};
        static int idx = 0;
        history[idx] = accMagnitude;
        idx = (idx + 1) % 20;
        
        float sum = 0;
        for (int i = 0; i < 20; i++) {
            sum += history[i];
        }
        float avg = sum / 20.0;
        threshold = avg + 2.0;  // 动态阈值 = 平均值 + 偏移量
    }
    
    bool nowAbove = (accMagnitude > threshold);
    
    if (nowAbove && !wasAbove) {
        lastStepTime = currentTime;
        wasAbove = nowAbove;
        return true;
    }
    
    wasAbove = nowAbove;
    return false;
}

