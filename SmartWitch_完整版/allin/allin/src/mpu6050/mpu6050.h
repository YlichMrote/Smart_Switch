#ifndef _MPU6050_H
#define _MPU6050_H

#include "Arduino.h"
#include "../SW_FreeRTOS/SW_FreeRTOS.h"
#include <Adafruit_MPU6050.h>
#include <FreeRTOS.h>
#include <task.h>
#include <string.h>

#define INTERVAL_BUFFER_SIZE 10 //环形缓冲区大小
// ========== 用户参数设置 ==========
#define STEP_LENGTH_CM 70.0     // 步长 (厘米)，请根据实际情况修改
#define MIN_PEAK_INTERVAL 250   // 最小峰值间隔 (毫秒)，防止高频噪声误判
// =================================


extern Adafruit_MPU6050 mpu;

// 姿态缓存
extern float mpu6050_roll;
extern float mpu6050_pitch;
extern float mpu6050_accFilt;   // 卡尔曼平滑后的合加速度（mg）

// ========== 变量声明 ==========
extern float Q_angle;   // 卡尔曼滤波参数：过程噪声协方差
extern float Q_bias;    // 卡尔曼滤波参数：偏置噪声协方差
extern float R_measure;  // 卡尔曼滤波参数：测量噪声协方差

extern float angle;                       // 状态变量：角度
extern float bias;                        // 状态变量：偏置
extern float rate;                        // 测量变量：角速度
extern float P[2][2];  // 协方差矩阵
extern float K[2];                              // 卡尔曼增益
extern float y, S;

extern unsigned long lastPeakTime;
extern unsigned long startTime;
extern int stepCount;
extern float totalDistance;
extern unsigned long lastPrintTime;

extern unsigned long intervalBuffer[];
extern int bufferIndex;
extern bool bufferFull;

void initBuffer();
void Task_CMPU6050(void *arg);
static void addToBuffer(unsigned long interval);
static unsigned long getAverageInterval();
static float KalmanFilter(float newAngle, float newRate, float dt);
static void calibrateSensor(void);
static bool detectStep(float accMagnitude);
#endif