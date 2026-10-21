#ifndef _CLOCK_H_
#define _CLOCK_H_

#include <NTPClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <string.h>

#include <Ticker.h>
#include <FreeRTOS.h>
#include <task.h>
#include "vars.h"

/* 单次连接尝试的超时时间：超时后由 Task_WiFi 自动重试，UI 全程不受影响 */
#define WIFI_CONNECT_TIMEOUT_MS 15000UL
/* 天气刷新周期（30 分钟）。首次联网成功后会立刻拉一次 */
#define WEATHER_REFRESH_MS      (30UL * 60UL * 1000UL)


void clock_init(void);
void clock_update(void);
void Task_WiFi(void *arg);
bool wifi_is_connected(void);

static void Get_Weather(void);
static String generateSuggestion(float temp, String weather);
static void parseWeatherJson(String json_str);
static void Get_Daily_Weather(void);
static void parseDailyJson(String json_str);

void IRAM_ATTR onTimer(void);

#endif