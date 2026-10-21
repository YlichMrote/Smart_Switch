#include "ble_uart.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>

#include "../prac1/vars.h"
#include "../prac1/parc.h"
#include "../prac1/screens.h"
#include "../prac1/eez-flow.h"
#include "../sht30/sht30.h"
#include "../bmp280/bmp280.h"
#include "../gy302/gy302.h"
#include "../heart/heart.h"
#include "../inmp441/inmp441.h"

// 按键引脚（与 Key.cpp 保持一致）
#define BLE_KEY_LEFT_PIN  41
#define BLE_KEY_RIGHT_PIN 42

// ============================ 全局状态 ============================
static BLEServer         *pServer  = NULL;
static BLECharacteristic *pTxChar  = NULL;   // ESP32 -> 客户端（notify）
static BLECharacteristic *pRxChar  = NULL;   // 客户端 -> ESP32（write）

static volatile bool bleConnected = false;
static volatile bool forceSync    = false;

/* 网页下发的播放请求：0=无 1=好的 2=我在
 * ★ 不能在 NimBLE 的写回调里直接播放音频 —— 那个回调跑在蓝牙协议栈自己的
 *   任务上下文里，而 playAudio() 是阻塞式 i2s_write（一段几百毫秒），
 *   会把协议栈任务卡住，轻则连接卡顿、重则超时掉线。
 *   这里只登记请求，真正播放交给 Task_BLE。 */
static volatile uint8_t audioReq = 0;

static SemaphoreHandle_t evtMutex = NULL;
static String  evtQueue  = "";               // 事件缓冲，以 '|' 分隔
static int16_t lastScreen = -1;
static uint8_t lastVoice  = 0xFF;
static bool    lastKeyL   = false;
static bool    lastKeyR   = false;

// ============================ 工具函数 ============================
// JSON 字符串转义
static void jsonEsc(String &out, const char *s) {
    if (s == NULL) return;
    while (*s) {
        char c = *s++;
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n' || c == '\r' || c == '\t') { out += ' '; }
        else if ((uint8_t)c < 0x20) { /* 丢弃其它控制字符 */ }
        else out += c;
    }
}

void BLE_PushEvent(const String &e) {
    if (evtMutex != NULL && xSemaphoreTake(evtMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    if (evtQueue.length() > 220) evtQueue = "";   // 防止异常堆积
    evtQueue += e;
    evtQueue += '|';
    if (evtMutex != NULL) xSemaphoreGive(evtMutex);
}

// 读取两个按键的当前电平：bit0=左 bit1=右
static uint8_t keyMask(void) {
    uint8_t m = 0;
    if (digitalRead(BLE_KEY_LEFT_PIN)  == LOW) m |= 0x01;
    if (digitalRead(BLE_KEY_RIGHT_PIN) == LOW) m |= 0x02;
    return m;
}

// ============================ 数据打包 ============================
static void buildPayload(String &j) {
    j = "";             // 长度清零但保留已分配的缓冲区，避免每帧重新分配
    j.reserve(1200);    // reserve() 内部自带“容量足够则直接返回”，开销可忽略

    String evt;
    if (evtMutex != NULL && xSemaphoreTake(evtMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        evt = evtQueue;
        evtQueue = "";
        xSemaphoreGive(evtMutex);
    }

    j += "{\"ts\":";             j += String(millis());
    j += ",\"evt\":\"";          jsonEsc(j, evt.c_str());              j += "\"";

    // ---- 环境：温湿度 / 气压 / 海拔 / 光照 ----
    j += ",\"env\":{\"t\":";     j += String(sht30_temp, 1);
    j += ",\"h\":";              j += String(sht30_hum, 1);
    j += ",\"p\":";              j += String(bmp280_press / 100.0f, 2);   // Pa -> hPa
    j += ",\"alt\":";            j += String(bmp280_alti, 1);
    j += ",\"lux\":";            j += String(gy302_lux, 1);
    j += ",\"light\":\"";        jsonEsc(j, get_var_lightness_text());  j += "\"}";

    // ---- 天气与出行建议 ----
    j += ",\"wx\":{\"now\":";    j += String(get_var_environmrnt_currenttemperature());
    j += ",\"min\":";            j += String(get_var_today_min_temperature());
    j += ",\"max\":";            j += String(get_var_today_max_temperature());
    j += ",\"text\":\"";         jsonEsc(j, get_var_weather_text());    j += "\"";
    j += ",\"sug\":\"";          jsonEsc(j, get_var_travel_suggestion()); j += "\"}";

    // ---- 主页：电量 / 步数 / 卡路里 / 日期时间 ----
    j += ",\"main\":{\"pwr\":";  j += String(get_var_main_power());
    j += ",\"step\":";           j += String(get_var_main_step());
    j += ",\"cal\":";            j += String(get_var_sport_cal());
    j += ",\"date\":\"";         jsonEsc(j, get_var_native_date());     j += "\"";
    j += ",\"time\":\"";         jsonEsc(j, get_var_native_time());     j += "\"";
    j += ",\"sec\":\"";          jsonEsc(j, get_var_native_second());   j += "\"}";

    // ---- 健康：心率 / 血氧 / 运行状态 / 信号强度 ----
    // st: 0=传感器未就绪 1=未检测到手指 2=测量中 3=读数正常
    // ir: 最近一次 IR 原始采样值（单点，跳得厉害）
    // dc: 窗口 IR 直流均值（反映手指贴合程度）
    // ac: 窗口 IR 峰峰值（反映脉动幅度，真正的“信号质量”指标）
    // irb/ire: 自适应基线 与 进入门限，用于现场判断手指判定是否正常
    //          （ire 明显高于 irb 才对；若 ire 已经贴着 irb 说明基线被抬高）
    j += ",\"hd\":{\"hr\":";     j += String(get_var_health_heartbeat());
    j += ",\"spo2\":";           j += String(get_var_health_oxygen());
    j += ",\"st\":";             j += String(Heart_GetState());
    j += ",\"ir\":";             j += String(Heart_GetIR());
    j += ",\"dc\":";             j += String(Heart_GetDC());
    j += ",\"ac\":";             j += String(Heart_GetAC());
    j += ",\"irb\":";            j += String(Heart_GetBase());
    j += ",\"ire\":";            j += String(Heart_GetEnterTh());
    j += ",\"on\":";             j += String(Heart_IsOnline() ? 1 : 0);
    j += "}";

    // ---- 运动：距离 / 时长 / 配速 / 卡路里 ----
    j += ",\"sp\":{\"dist\":";   j += String(get_var_sport_distance(), 2);
    j += ",\"t\":";              j += String(get_var_sport_time());
    j += ",\"pace\":";           j += String(get_var_sport_pace());
    j += ",\"cal\":";            j += String(get_var_sport_cal());      j += "}";

    // ---- 系统状态 ----
    j += ",\"sys\":{\"scr\":";   j += String(eez_flow_get_current_screen());
    j += ",\"up\":";             j += String(millis() / 1000);
    j += ",\"rssi\":";           j += String(WiFi.RSSI());
    j += ",\"wifi\":";           j += String(WiFi.status() == WL_CONNECTED ? 1 : 0);
    j += ",\"key\":";            j += String(keyMask());
    j += ",\"voice\":";          j += String(INMP441_GetState());
    j += ",\"heard\":\"";        jsonEsc(j, INMP441_GetHeard());        j += "\"";
    j += ",\"cmd\":\"";          jsonEsc(j, INMP441_GetLastCmd());      j += "\"";
    j += ",\"mtu\":";            j += String(BLEDevice::getMTU());
    // 任务体检汇总量：全任务最小剩余栈(B) / 本窗口最大周期超时(ms)
    j += ",\"stkmin\":";         j += String(TaskMon_MinStackFree());
    j += ",\"jit\":";            j += String(TaskMon_MaxLagMs());
    j += "}}";
}

// 分块发送：长 JSON 拆成多个通知包，网页端按“可解析即完整”规则重组
bool BLE_NotifyText(const String &text) {
    if (!bleConnected || pTxChar == NULL) return false;

    const uint8_t *buf = (const uint8_t *)text.c_str();
    size_t total = text.length();
    size_t off   = 0;

    // 按协商到的 MTU 决定分片大小（ATT 头部占 3 字节）
    size_t chunk = (size_t)BLEDevice::getMTU();
    chunk = (chunk > 3) ? (chunk - 3) : 20;
    if (chunk > BLE_CHUNK_MAX) chunk = BLE_CHUNK_MAX;
    if (chunk < 20)            chunk = 20;

    while (off < total) {
        if (!bleConnected) return false;
        size_t n = total - off;
        if (n > chunk) n = chunk;

        pTxChar->setValue(buf + off, n);
        pTxChar->notify();
        off += n;

        if (off < total) vTaskDelay(pdMS_TO_TICKS(8));   // 给协议栈留出发送时间
    }
    return true;
}

// ============================ 指令解析 ============================
static void applyScreenStep(int8_t dir) {
    int16_t cur = eez_flow_get_current_screen();
    if (dir < 0) cur = (cur <= _SCREEN_ID_FIRST) ? _SCREEN_ID_LAST  : cur - 1;
    else         cur = (cur >= _SCREEN_ID_LAST)  ? _SCREEN_ID_FIRST : cur + 1;
    Parc_RequestScreen(cur);
}

static void handleCommand(String cmd) {
    cmd.trim();
    if (cmd.length() == 0) return;

    Serial.print("[BLE] 收到指令: ");
    Serial.println(cmd);
    BLE_PushEvent("rx:" + cmd);

    if (cmd == "ping" || cmd == "sync") {
        forceSync = true;
    }
    else if (cmd.startsWith("screen:")) {
        int16_t id = (int16_t)cmd.substring(7).toInt();
        if (id >= _SCREEN_ID_FIRST && id <= _SCREEN_ID_LAST) Parc_RequestScreen(id);
    }
    else if (cmd == "key:left") {
        applyScreenStep(-1);
    }
    else if (cmd == "key:right") {
        applyScreenStep(+1);
    }
    else if (cmd == "audio:haode") {
        audioReq = 1;      // 交给 Task_BLE 播，别卡住蓝牙协议栈
    }
    else if (cmd == "audio:wozai") {
        audioReq = 2;
    }
    else {
        Serial.println("[BLE] 未知指令");
    }
}

// ============================ 回调 ============================
class BleRxCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) {
        // 3.x 返回 String，2.x 返回 std::string，统一走 c_str()
        String v = String(pChar->getValue().c_str());
        if (v.length() > 0) handleCommand(v);
    }
};

class BleServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *pSrv) {
        // 注意：这里刻意不做任何语音播报 —— 连接蓝牙是静默行为，
        // 播报音会打扰用户，语音只在收到 audio:haode / audio:wozai 指令时播放。
        bleConnected = true;
        forceSync    = true;
        Serial.println("[BLE] 客户端已连接（静默，无语音播报）");
    }

    void onDisconnect(BLEServer *pSrv) {
        bleConnected = false;
        Serial.println("[BLE] 客户端断开，重新开启广播");
        vTaskDelay(pdMS_TO_TICKS(300));
        pSrv->startAdvertising();
    }
};

// ============================ 初始化 ============================
void BLE_Init(void) {
    if (evtMutex == NULL) evtMutex = xSemaphoreCreateMutex();

    Serial.println("[BLE] 正在初始化...");
    BLEDevice::init(BLE_DEVICE_NAME);
    BLEDevice::setMTU(517);                       // 争取更大的单包容量

    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new BleServerCallbacks());

    BLEService *pSvc = pServer->createService(BLE_SVC_UUID);

    pTxChar = pSvc->createCharacteristic(BLE_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    pTxChar->addDescriptor(new BLE2902());        // CCCD，浏览器订阅通知必需

    pRxChar = pSvc->createCharacteristic(BLE_RX_UUID,
                    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    pRxChar->setCallbacks(new BleRxCallbacks());

    pSvc->start();

    BLEAdvertising *pAdv = BLEDevice::getAdvertising();

    // ---------- 广播包配置（这里曾经导致“搜不到设备”，务必按注释改） ----------
    // BLE 广播包上限 31 字节。128 位服务 UUID 占 18 字节，加上设备名(12B)
    // 与 flags(3B) 共 33 字节 → 超限。NimBLE 检测到放不下时，会自动把
    // 设备名搬到 scan response；而桌面浏览器（Chrome/Edge）按名字过滤时
    // 只读广播包、读不到 scan response 里的名字，结果就是搜不到设备。
    // 因此：不要调用 addServiceUUID —— 去掉它后广播包仅 flags(3) + 名字(12)
    // = 15 字节，设备名一定留在广播包里。
    // UUID 不写进广播不影响连接：网页用 optionalServices 声明后，GATT 服务
    // 发现照样能拿到 6E400001。
    pAdv->setMinInterval(0x20);
    pAdv->setMaxInterval(0x30);
    pAdv->setName(BLE_DEVICE_NAME);
    BLEDevice::startAdvertising();

    vTaskDelay(pdMS_TO_TICKS(200));
    Serial.print("[BLE] 设备名: ");
    Serial.println(BLE_DEVICE_NAME);
    Serial.print("[BLE] 广播状态: ");
    Serial.println(pAdv->isAdvertising() ? "正在广播 OK" : "未广播 FAIL");
    Serial.print("[BLE] 协商 MTU: ");
    Serial.println(BLEDevice::getMTU());
}

bool BLE_IsConnected(void) { return bleConnected; }
void BLE_RequestSync(void) { forceSync = true; }

// ============================ 任务 ============================
void Task_BLE(void *arg) {
    static String frame;

    vTaskDelay(pdMS_TO_TICKS(1000));

    for (;;) {
        // 名义周期 50 ms；本任务在播报音频 / 发送多分片数据帧时会拉长，
        // 体检表里看到的最大周期因此会明显大于 50 ms，属预期。
        TaskMon_Tick(TM_BLE, 50);

        // 网页手动点的语音播放在这里执行（不占用蓝牙协议栈任务上下文）
        uint8_t ar = audioReq;
        if (ar) {
            audioReq = 0;
            if (ar == 1) Audio_PlayHaode();
            else         Audio_PlayWozai();
        }

        if (bleConnected) {
            // ---- 状态变化检测，生成事件 ----
            int16_t scr = eez_flow_get_current_screen();
            if (scr != lastScreen) {
                if (lastScreen >= 0) BLE_PushEvent("screen:" + String(scr));
                lastScreen = scr;
            }

            bool kl = (digitalRead(BLE_KEY_LEFT_PIN)  == LOW);
            bool kr = (digitalRead(BLE_KEY_RIGHT_PIN) == LOW);
            if (kl && !lastKeyL) BLE_PushEvent("key:L");
            if (kr && !lastKeyR) BLE_PushEvent("key:R");
            lastKeyL = kl;
            lastKeyR = kr;

            uint8_t v = INMP441_GetState();
            if (v != lastVoice) {
                if (lastVoice != 0xFF) BLE_PushEvent("voice:" + String(v));
                lastVoice = v;
            }

            // ---- 周期上报 ----
            static uint32_t lastSend = 0;
            if (forceSync || (millis() - lastSend) >= BLE_REPORT_INTERVAL) {
                forceSync = false;
                lastSend  = millis();
                buildPayload(frame);
                BLE_NotifyText(frame);
            }
        } else {
            forceSync  = false;
            lastScreen = -1;
            lastVoice  = 0xFF;
            lastKeyL   = false;
            lastKeyR   = false;
        }

        TaskMon_Report();      // 每 10 秒打印一次任务时序体检表（串口）
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
