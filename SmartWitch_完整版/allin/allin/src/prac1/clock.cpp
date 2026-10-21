#include "clock.h"
#include "../inmp441/inmp441.h"

const char *ssid     = "iQOO Z10 Turbo+";
const char *password = "7i5e9tdwvpr4cei";
const char* API_KEY   = "SvUS9ZYjHZp6A88IE";
const char* CITY      = "哈尔滨";
const char* LANGUAGE  = "zh-Hans";


float current_temp;       // 实时温度
float today_min_temp;     // 今日最低温
float today_max_temp;     // 今日最高温
char weather_text1[10] = {0};      // 天气状况：晴 / 多云 / 雨 / 雪
char travel_suggestion1[50] = {0}; // 出行建议

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP,"ntp.aliyun.com",28800,86400000);

Ticker secondTicker;
volatile bool time_update = false;

char buffer[50];
time_t timestamp;   //时间戳
struct tm *timeinfo;

/* ----------------------------------------------------------------------------
 * NTP 同步周期
 * ----------------------------------------------------------------------------
 * NTPClient::getEpochTime() 内部是「上次同步的基准 + (millis() - 上次同步时刻)」，
 * 也就是说它自己会用 millis() 推算秒数，本地走时是准的。
 * 旧实现在 clock_update() 里每秒调用一次 timeClient.update()，
 * 等价于每秒往 ntp.aliyun.com 发一个 UDP 包 —— WiFi 射频每秒都要收发，
 * 几乎无法进入省电状态，是持续性发热的一大来源。
 * 改成每 10 分钟同步一次，秒针显示完全不受影响。
 * --------------------------------------------------------------------------*/
#define NTP_SYNC_INTERVAL 600000UL      // 10 分钟
static uint32_t lastNtpSync = 0;

/* ==================== 网络状态 ====================
 * wifi_up / time_synced / weather_pending 由 Task_WiFi（core0）写，
 * 由 clock_update()（core1，LVGL 所在任务）读。
 * 天气数据不在联网任务里直接 set_var，避免和 LVGL 刷新线程竞争同一块缓冲。
 */
static volatile bool wifi_up         = false;   // 已连上 AP 并拿到 IP
static volatile bool time_synced     = false;   // NTP 已成功对过一次
static volatile bool weather_pending = false;   // 天气数据已就绪，待推给 UI

bool wifi_is_connected(void)
{
    return wifi_up;
}

/* 把天气数据推给 EEZ 变量 —— 只能在 UI 线程（clock_update）里调用 */
static void weather_push_to_ui(void)
{
    set_var_environmrnt_currenttemperature((int32_t)current_temp);
    set_var_weather_text(weather_text1);
    set_var_travel_suggestion(travel_suggestion1);
    set_var_today_min_temperature((int32_t)today_min_temp);
    set_var_today_max_temperature((int32_t)today_max_temp);
    Serial.println("[UI] 天气变量已更新");
}

void clock_init(void)
{
    /* 只做配置和发起连接，绝不在这里等待。
     * 真正的「等连接 / 首次对时 / 拉天气」全部交给 Task_WiFi 后台并发处理，
     * 这样屏幕点亮之后到 FreeRTOS_Start() 之间不会被网络卡住。 */
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);

    timeClient.begin();

    WiFi.begin(ssid, password);
    Serial.print("[WiFi] 后台连接中: ");
    Serial.println(ssid);

    secondTicker.attach(1.0, onTimer);
}

/* ==================== 后台联网任务（core0） ====================
 * 职责：
 *   1. 等 Wi-Fi 连上（带超时，失败自动重试，全程不卡 UI）
 *   2. 连上后立即做首次 NTP 对时
 *   3. 通知语音模块去拿百度 token（必须联网才能拿）
 *   4. 首次 + 每 30 分钟刷新天气
 *   5. 掉线后自动重连
 *
 * 之后的 NTP 校时仍由 clock_update() 按 10 分钟节流继续维护。
 */
void Task_WiFi(void *arg)
{
    uint32_t lastWeather = millis() - WEATHER_REFRESH_MS;  // 联网后立刻拉一次
    uint32_t lastBegin   = millis();

    for (;;) {
        /* 名义周期：联网时 1000 ms、未联网时 500 ms（由下面的 vTaskDelay 决定）。
         * 实测均值减去名义周期，就是这个任务联网/重试时真正吃掉的 CPU 时间。 */
        TaskMon_Tick(TM_WIFI, (WiFi.status() == WL_CONNECTED) ? 1000 : 500);

        if (WiFi.status() == WL_CONNECTED) {
            if (!wifi_up) {
                wifi_up = true;
                Serial.print("[WiFi] 已连接, IP = ");
                Serial.println(WiFi.localIP());

                if (timeClient.forceUpdate()) {
                    time_synced = true;
                    Serial.println("[NTP] 首次对时成功");
                } else {
                    Serial.println("[NTP] 首次对时失败, 稍后由 clock_update 重试");
                }

                INMP441_NetworkReady();          // 联网后才去拿百度 token

                lastWeather = millis() - WEATHER_REFRESH_MS;
            }

            if (millis() - lastWeather >= WEATHER_REFRESH_MS) {
                lastWeather = millis();
                Serial.println("[NET] 拉取天气...");
                Get_Weather();
                weather_pending = true;
            }

            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            if (wifi_up) {
                wifi_up = false;
                time_synced = false;
                Serial.println("[WiFi] 连接断开, 准备重连");
            }

            /* 超过超时时间还没连上 -> 重新发起一次连接 */
            if (millis() - lastBegin >= WIFI_CONNECT_TIMEOUT_MS) {
                Serial.print("[WiFi] 连接超时, 重试: ");
                Serial.println(ssid);
                WiFi.begin(ssid, password);
                lastBegin = millis();
            }

            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

void IRAM_ATTR onTimer() {
  time_update = true;  
}

void clock_update(void)
{
    /* 天气数据到了：在这里（UI 线程）推进 LVGL 变量 */
    if (weather_pending) {
        weather_pending = false;
        weather_push_to_ui();
    }

    if (!time_update) return;
    time_update = false;

    /* 超过同步周期才真正发网络请求；其余时间靠 millis() 本地推算。
     * 另外 timeClient.update() 在未联网时会阻塞约 1 秒，
     * 会拖慢同线程的 LVGL 刷新，所以再用 wifi_up 兜一道闸。 */
    if (wifi_up && millis() - lastNtpSync >= NTP_SYNC_INTERVAL) {
        lastNtpSync = millis();
        if (timeClient.update()) {
            time_synced = true;
        } else if (!time_synced) {
            Serial.println("[Clock] NTP 同步失败，继续等待");
        }
    }

    /* 一次都没对上时显示占位符，而不是把 1970 年的时间戳刷到界面上 */
    if (!time_synced) {
        set_var_native_date("未连接WiFi");
        set_var_native_time("--\n--");
        set_var_native_second("--");
        return;
    }

    timestamp = timeClient.getEpochTime();
    timeinfo = localtime(&timestamp);

    sprintf(buffer,"%04d年%02d月%02d日 周%1d",timeinfo->tm_year+1900,timeinfo->tm_mon+1,timeinfo->tm_mday,timeinfo->tm_wday);
    set_var_native_date(buffer);

    sprintf(buffer,"%02d\n%02d",timeinfo->tm_hour,timeinfo->tm_min);
    set_var_native_time(buffer);

    sprintf(buffer,"%02d",timeinfo->tm_sec+1);
    set_var_native_second(buffer);

    //Serial.print( buffer ); 
}

void Get_Weather(void)
{
    String url = "https://api.seniverse.com/v3/weather/now.json?key=";
    url += API_KEY;
    url += "&location=" + String(CITY);
    url += "&language=" + String(LANGUAGE);
    url += "&unit=c";

    HTTPClient http;
    http.begin(url);
    http.setTimeout(10000);

    int http_code = http.GET();
    if (http_code == HTTP_CODE_OK) {
      String response = http.getString();
      Serial.println("\n===== 原始API响应 =====");
      Serial.println(response);

      parseWeatherJson(response);
      http.end();
      Get_Daily_Weather();
    } 
    else {
      Serial.println("\n请求失败,状态码: " + String(http_code));
      http.end();
    }

    
}

void generateSuggestion(float temp, const char* weather, char* out_buf, size_t buf_len) {
  if (strstr(weather, "雨") != NULL) {
    strncpy(out_buf, "出行请带好雨具，注意路滑呦~", buf_len - 1);
  } else if (strstr(weather, "雪") != NULL) {
    strncpy(out_buf, "雪天路滑，注意保暖和行车安全呦~", buf_len - 1);
  } else if (strstr(weather, "雾") != NULL) {
    strncpy(out_buf, "雾天能见度低，出行注意安全呦~", buf_len - 1);
  } else if (strstr(weather, "晴") != NULL) {
    if (temp < 0) strncpy(out_buf, "天气晴好，注意保暖，适宜出行哦~", buf_len - 1);
    else if (temp < 15) strncpy(out_buf, "天气晴好，适宜户外活动哦~", buf_len - 1);
    else if (temp < 28) strncpy(out_buf, "天气舒适，非常适合出行哦~", buf_len - 1);
    else strncpy(out_buf, "天气炎热，注意防晒补水呦~", buf_len - 1);
  } else if (strstr(weather, "云") != NULL || strstr(weather, "阴") != NULL) {
    strncpy(out_buf, "多云/阴天，适合出行，注意温差呦~", buf_len - 1);
  } else if (strstr(weather, "风") != NULL) {
    strncpy(out_buf, "大风天气，注意防风，尽量减少外出", buf_len - 1);
  } else if (strstr(weather, "沙") != NULL || strstr(weather, "尘") != NULL) {
    strncpy(out_buf, "沙尘天气，建议佩戴口罩，减少外出", buf_len - 1);
  } else {
    strncpy(out_buf, "天气平稳，正常出行即可哦~", buf_len - 1);
  }
  out_buf[buf_len - 1] = '\0'; // 强制加结束符，防止越界
}

void parseWeatherJson(String json_str) {
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, json_str);

  if (error) {
    Serial.println("JSON解析失败: " + String(error.c_str()));
    return;
  }

  JsonObject now = doc["results"][0]["now"];

  current_temp = now["temperature"].as<float>();

  const char* weather_text = now["text"].as<const char*>();
  strncpy(weather_text1, weather_text, sizeof(weather_text1) - 1);
  weather_text1[sizeof(weather_text1) - 1] = '\0';

 

  // ================= 串口输出 ====================
  Serial.print("温度: ");
  Serial.println(current_temp);
  

  Serial.print("天气状况: ");
  Serial.println(weather_text1);

  

  Serial.println("============================");

  /* 这里不直接写 UI 变量：parseWeatherJson 跑在 Task_WiFi(core0)，
   * 直接 set_var 会与 UI 线程的 LVGL 刷新抢同一块缓冲。
   * 顺便先给个兜底建议，即使逐日天气请求失败也有内容可显示。 */
  generateSuggestion(current_temp, weather_text1, travel_suggestion1, sizeof(travel_suggestion1));
}


void Get_Daily_Weather(void)
{
    String url = "https://api.seniverse.com/v3/weather/daily.json?key=";
    url += API_KEY;
    url += "&location=" + String(CITY);
    url += "&language=" + String(LANGUAGE);
    url += "&unit=c";
    url += "&days=1";

    HTTPClient http;
    http.begin(url);
    http.setTimeout(5000);

    int http_code = http.GET();
    if (http_code == HTTP_CODE_OK) {
      String response = http.getString();
      parseDailyJson(response);
    } 
    else {
      Serial.println("\n逐日天气请求失败,状态码: " + String(http_code));
    }
    http.end();
}

void parseDailyJson(String json_str) {
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, json_str);
  if (error) {
    Serial.println("逐日JSON解析失败: " + String(error.c_str()));
    return;
  }

  JsonObject today = doc["results"][0]["daily"][0];
  today_min_temp = today["low"].as<float>();
  today_max_temp = today["high"].as<float>();
  Serial.print("今日温度范围: ");
  Serial.print(today_min_temp, 2);
  Serial.print("℃ ~ ");
  Serial.print(today_max_temp, 2);
  Serial.println("℃");


  // 重新生成出行建议（用最新温幅）
  generateSuggestion(current_temp, weather_text1, travel_suggestion1, sizeof(travel_suggestion1));
  Serial.print("出行建议: ");
  Serial.println(travel_suggestion1);

  /* 同上：数据先落到全局变量，由 clock_update() 在 UI 线程里推给 LVGL */
}
