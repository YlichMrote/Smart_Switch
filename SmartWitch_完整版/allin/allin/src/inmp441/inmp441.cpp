#include "inmp441.h"
#include <math.h>

/* ============================================================================
 * SmartWitch 语音模块（重写版）
 * ----------------------------------------------------------------------------
 * 方案不变：INMP441 采集 → 百度语音开放平台（短语音识别 server_api）→ 本地匹配
 * 指令 → 切屏 / 播报。
 *
 * 旧实现的两个致命问题：
 *   1) 每轮 i2s_read 带 200ms 超时，实际一次只拿到约 0.2 秒音频就立刻发一次
 *      云端请求。唤醒词「小度」有 0.4~0.6 秒，被切成碎片后大概率跨在两次
 *      请求的边界上 → 识别不出来 → 用户反复喊，感觉又慢又不准；而每秒 5 次
 *      的 base64 + HTTP 把 CPU 和射频拉满 → 发烫。
 *   2) 在语音任务里直接调 eez_flow_set_screen() 操作 LVGL，跨任务访问 UI 不
 *      安全（工程约定：LVGL 只允许在 Task_Parc 上下文执行）。
 *
 * 新实现：
 *   · 40ms 小块采集 + 自适应噪声门限做本地 VAD，只在「真的有人说话」时才请求
 *   · 一句话完整录下来（尾部静音 0.5~0.7 秒收句）再整段送云端 → 一次请求
 *     就是完整语句，识别率大幅提高，请求次数下降 95% 以上
 *   · 一阶高通去掉低频噪声 + RMS 增益归一化，提升信噪比
 *   · base64 直接写进 PSRAM，用二进制 POST，零大 String 堆抖动
 *   · token 失效（err_no 110/111）自动重取
 *   · 切屏改走 Parc_RequestScreen()，交给 UI 任务执行
 * ==========================================================================*/

/* ============================ 一、云端凭据 ============================ */
static const char* BAIDU_API_KEY    = "gnEpjNTeZetWtmmdN8x5cgNZ";
static const char* BAIDU_SECRET_KEY = "8KyIWg6MgVi4Lm61hUpbMzFVkv3x68i7";
static const char* BAIDU_TOKEN_URL  = "https://aip.baidubce.com/oauth/2.0/token?grant_type=client_credentials&client_id=";
static const char* BAIDU_ASR_URL    = "http://vop.baidu.com/server_api";

/* ============================ 二、可调参数 ============================ */
#define VOICE_SR              16000                                  // 采样率（百度要求 16k）
#define BLOCK_MS              40                                     // VAD 块长
#define BLOCK_SAMPLES         (VOICE_SR * BLOCK_MS / 1000)           // 640 采样
#define BLOCK_RAW_BYTES       (BLOCK_SAMPLES * 4)                    // I2S 32bit 槽 → 2560 B

#define UTTER_MAX_SEC         5.5f                                   // 单段最长录音
#define UTTER_MAX_SAMPLES     ((int)(VOICE_SR * UTTER_MAX_SEC))      // 88000 采样
#define WAKE_MAX_SAMPLES      (VOICE_SR * 2)                         // 唤醒词最长 2 秒
#define UTTER_MIN_SAMPLES     2400                                   // < 0.15 秒当噪声丢弃

#define TAIL_SIL_WAKE_MS      500                                    // 唤醒段：尾静音 0.5 秒收句
#define TAIL_SIL_CMD_MS       700                                    // 指令段：尾静音 0.7 秒收句
#define CMD_WAIT_MS           6000                                   // 唤醒后等指令窗口

#define NOISE_FLOOR_K         2.4f                                   // 门限 = 环境底噪 × K
#define NOISE_FLOOR_ABS       140.0f                                 // 门限绝对下限（RMS）
#define TARGET_RMS            4200.0f                                // 增益归一化目标 RMS
#define GAIN_MAX              20.0f                                  // 增益上限（避免放大纯噪声）
#define HPF_A                 0.97f                                  // 一阶高通系数 ≈ 80Hz @16k

#define JSON_BUF_SIZE         (UTTER_MAX_SAMPLES * 2 / 3 * 4 + 512)  // JSON + base64 组装区

/* ============================ 三、运行状态 ============================ */
static int32_t* rawBuf  = NULL;   // I2S 原始块（32bit 槽）
static int16_t* s16Buf  = NULL;   // 转换 + 滤波后的块
static int16_t* pcmBuf  = NULL;   // 录音累积区
static char*    jsonBuf = NULL;   // JSON 组装区
static bool     voiceReady = false;

static float noiseFloor = 300.0f; // 环境噪声底噪（RMS，自适应跟踪）
static float hpfY = 0.0f;         // 一阶高通状态
static float hpfX = 0.0f;

static String baiduToken = "";
static String lastHeardText   = "";
static String lastCommandText = "";

// 对外状态：0=待唤醒 1=已唤醒(等指令) 2=识别中
static volatile uint8_t vState = 0;

uint8_t     INMP441_GetState(void)   { return vState; }
const char* INMP441_GetHeard(void)   { return lastHeardText.c_str();   }
const char* INMP441_GetLastCmd(void) { return lastCommandText.c_str(); }

/* ============================ 四、百度 Token ============================ */
static void voiceTokenRefresh(void)
{
    HTTPClient http;
    String url = String(BAIDU_TOKEN_URL) + BAIDU_API_KEY +
                 "&client_secret=" + BAIDU_SECRET_KEY;
    http.begin(url);
    http.setConnectTimeout(3000);
    http.setTimeout(6000);

    int code = http.GET();
    if (code == 200) {
        String payload = http.getString();
        int s = payload.indexOf("\"access_token\":\"");
        if (s != -1) {
            s += 16;
            int e = payload.indexOf('"', s);
            if (e > s) {
                baiduToken = payload.substring(s, e);
                Serial.println("[Voice] token 获取成功");
            }
        }
    } else {
        Serial.printf("[Voice] token 获取失败，错误码: %d\n", code);
    }
    http.end();
}

/* ============================ 五、I2S 初始化 ============================ */
static void voiceI2SInit(void)
{
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = VOICE_SR,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 256,     // 8 × 256 帧 = 128ms 缓冲（越大延迟越高）
        .use_apll             = true
    };
    i2s_pin_config_t pin = {
        .bck_io_num   = I2S_SCK_PIN,
        .ws_io_num    = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = I2S_SD_PIN
    };
    i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin);
    Serial.println("[Voice] I2S 麦克风初始化完成");
}

/* 清空 DMA 里的残留音频（例如播报「我在」期间堆积的旧数据） */
static void voiceFlushRx(void)
{
    size_t got = 0;
    for (int i = 0; i < 16; i++) {
        if (i2s_read(I2S_NUM_0, rawBuf, BLOCK_RAW_BYTES, &got, 0) != ESP_OK || got == 0) break;
    }
}

/* ============================================================================
 * 六、采集一块：去直流 → 取 16bit → 一阶高通
 * ----------------------------------------------------------------------------
 * INMP441 输出 24bit 数据左对齐在 32bit 槽里，右移 16 位取高 16 位即可。
 * 直流用「块内均值」估计（比整段均值更贴近实时漂移），再串一级高通滤掉
 * 空调 / 风扇这类 80Hz 以下的低频轰鸣。
 * ==========================================================================*/
static int voiceFillBlock(int16_t* dst)
{
    size_t got = 0;
    if (i2s_read(I2S_NUM_0, rawBuf, BLOCK_RAW_BYTES, &got, pdMS_TO_TICKS(200)) != ESP_OK || got < 4)
        return 0;

    int     n   = (int)(got / 4);
    int64_t sum = 0;
    for (int i = 0; i < n; i++) sum += (int64_t)rawBuf[i];
    int32_t dc = (int32_t)(sum / n);

    for (int i = 0; i < n; i++) {
        float x = (float)((int32_t)((rawBuf[i] - dc) >> 16));
        hpfY = HPF_A * (hpfY + x - hpfX);     // y[n] = a·(y[n-1] + x[n] − x[n-1])
        hpfX = x;

        int32_t y = (int32_t)hpfY;
        if (y >  32767) y =  32767;
        if (y < -32768) y = -32768;
        dst[i] = (int16_t)y;
    }
    return n;
}

static float voiceRMS(const int16_t* buf, int n)
{
    if (n <= 0) return 0.0f;
    double e = 0.0;
    for (int i = 0; i < n; i++) e += (double)buf[i] * buf[i];
    return sqrtf((float)(e / n));
}

/* 增益归一化：把整段语音 RMS 抬到 TARGET_RMS，小信号也能被云端听清。
 * 只放大不衰减，超限钳位，避免削顶失真。 */
static void voiceApplyGain(int16_t* buf, int n)
{
    float rms = voiceRMS(buf, n);
    if (rms < 1.0f || rms >= TARGET_RMS) return;

    float g = TARGET_RMS / rms;
    if (g > GAIN_MAX) g = GAIN_MAX;
    if (g <= 1.05f)   return;

    for (int i = 0; i < n; i++) {
        int32_t v = (int32_t)(buf[i] * g);
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        buf[i] = (int16_t)v;
    }
}

/* ============================================================================
 * 七、录一句话（本地 VAD 控制，期间不发任何网络请求）
 * ----------------------------------------------------------------------------
 * maxWaitMs = 0 表示「无限等」（用于等待唤醒词）；
 * 其余情况在开口之前超过 maxWaitMs 就返回 0。
 * 返回累积到的样本数（已去尾静音 + 做过增益归一化）；0 表示这段没有有效语音。
 * ==========================================================================*/
static int voiceRecord(int16_t* dst, int maxSamples, uint32_t maxWaitMs, uint32_t tailSilMs)
{
    voiceFlushRx();

    int      n       = 0;
    bool     started = false;
    int      silBlk  = 0;
    uint32_t t0      = millis();

    while (n < maxSamples) {
        // 还没开口就超时了
        if (!started && maxWaitMs && (millis() - t0) > maxWaitMs) return 0;

        int got = voiceFillBlock(s16Buf);
        if (got <= 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }

        // 自适应底噪：安静时快速跟随下降，有语音时慢速上升（防止把语音算进底噪）
        float rms = voiceRMS(s16Buf, got);
        noiseFloor += (rms - noiseFloor) * ((rms < noiseFloor) ? 0.25f : 0.002f);

        float thr = noiseFloor * NOISE_FLOOR_K;
        if (thr < NOISE_FLOOR_ABS) thr = NOISE_FLOOR_ABS;
        bool voiced = (rms > thr);

        if (!started) {
            if (!voiced) continue;          // 还没开口，什么都不做
            started = true;
            t0      = millis();             // 从开口重新计时
            silBlk  = 0;
        }

        int copy = got;
        if (n + copy > maxSamples) copy = maxSamples - n;
        memcpy(dst + n, s16Buf, (size_t)copy * sizeof(int16_t));
        n += copy;

        if (voiced) {
            silBlk = 0;
        } else if (++silBlk * BLOCK_MS >= (int)tailSilMs) {
            break;                          // 尾部静音够了 → 认为一句话说完了
        }
    }

    if (!started) return 0;

    n -= silBlk * BLOCK_SAMPLES;            // 掐掉尾部静音
    if (n > maxSamples) n = maxSamples;
    if (n < UTTER_MIN_SAMPLES) return 0;

    voiceApplyGain(dst, n);
    return n;
}

/* ============================ 八、base64 编码 ============================ */
static const char B64TAB[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64Encode(const uint8_t* src, size_t len, char* dst)
{
    size_t i = 0, o = 0;
    while (i + 3 <= len) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | src[i + 2];
        dst[o++] = B64TAB[(v >> 18) & 0x3F];
        dst[o++] = B64TAB[(v >> 12) & 0x3F];
        dst[o++] = B64TAB[(v >>  6) & 0x3F];
        dst[o++] = B64TAB[ v        & 0x3F];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)src[i] << 16;
        dst[o++] = B64TAB[(v >> 18) & 0x3F];
        dst[o++] = B64TAB[(v >> 12) & 0x3F];
        dst[o++] = '=';
        dst[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8);
        dst[o++] = B64TAB[(v >> 18) & 0x3F];
        dst[o++] = B64TAB[(v >> 12) & 0x3F];
        dst[o++] = B64TAB[(v >>  6) & 0x3F];
        dst[o++] = '=';
    }
    return o;
}

/* ============================================================================
 * 九、云端识别（JSON 直接组装进 PSRAM，二进制 POST）
 * ==========================================================================*/
static String voiceRecognize(const int16_t* pcm, int samples)
{
    if (baiduToken.length() == 0 || !jsonBuf) return "";

    int pcmBytes = samples * 2;

    int head = snprintf(jsonBuf, 256,
        "{\"format\":\"pcm\",\"rate\":%d,\"dev_pid\":1537,\"channel\":1,"
        "\"token\":\"%s\",\"cuid\":\"ESP32_CLIENT\",\"len\":%d,\"speech\":\"",
        VOICE_SR, baiduToken.c_str(), pcmBytes);
    if (head <= 0) return "";

    size_t b64n  = b64Encode((const uint8_t*)pcm, (size_t)pcmBytes, jsonBuf + head);
    size_t total = (size_t)head + b64n;
    jsonBuf[total++] = '"';
    jsonBuf[total++] = '}';

    HTTPClient http;
    http.begin(BAIDU_ASR_URL);
    http.setConnectTimeout(2500);
    http.setTimeout(6000);
    http.addHeader("Content-Type", "application/json");

    int    code = http.POST((uint8_t*)jsonBuf, total);
    String text = "";

    if (code == 200) {
        String resp = http.getString();
        if (resp.indexOf("\"err_no\":0") != -1) {
            int s = resp.indexOf("\"result\":[\"");
            if (s != -1) {
                s += 11;
                int e = resp.indexOf("\"]", s);
                if (e > s) text = resp.substring(s, e);
            }
        } else if (resp.indexOf("\"err_no\":110") != -1 || resp.indexOf("\"err_no\":111") != -1) {
            Serial.println("[Voice] token 已失效，自动重新获取");
            voiceTokenRefresh();
        } else {
            Serial.println("[Voice] 识别失败: " + resp.substring(0, 140));
        }
    } else {
        Serial.printf("[Voice] HTTP 请求失败: %d\n", code);
    }
    http.end();

    if (text.length()) Serial.println("[Voice] 识别结果: " + text);
    return text;
}

/* ============================ 十、唤醒词 / 指令匹配 ============================ */
static bool voiceIsWakeWord(const String& s)
{
    // 云端对「小度」常返回同音字，做一次容错
    static const char* kw[] = { "小度", "小杜", "小肚", "晓度", "小渡", "小读", "小笃", "小都", "小赌" };
    for (size_t i = 0; i < sizeof(kw) / sizeof(kw[0]); i++) {
        if (s.indexOf(kw[i]) != -1) return true;
    }
    return false;
}

static void voiceExecuteCommand(const String& cmd)
{
    Serial.println("[Voice] 命中指令: " + cmd);
    lastCommandText = cmd;

    int16_t screen = -1;

    if (cmd.indexOf("温度") != -1 || cmd.indexOf("湿度") != -1 || cmd.indexOf("天气") != -1 ||
        cmd.indexOf("气温") != -1 || cmd.indexOf("气压") != -1 || cmd.indexOf("大气压") != -1 ||
        cmd.indexOf("海拔") != -1 || cmd.indexOf("环境") != -1) {
        screen = SCREEN_ID_ENVIRONMENT;
    }
    else if (cmd.indexOf("光照") != -1 || cmd.indexOf("亮度") != -1 || cmd.indexOf("光强") != -1 ||
             cmd.indexOf("光线") != -1) {
        screen = SCREEN_ID_LIGHT;
    }
    else if (cmd.indexOf("血氧") != -1 || cmd.indexOf("心率") != -1 || cmd.indexOf("心跳") != -1 ||
             cmd.indexOf("脉搏") != -1 || cmd.indexOf("健康") != -1 || cmd.indexOf("身体") != -1 ||
             cmd.indexOf("自己") != -1) {
        screen = SCREEN_ID_HEALTH;
    }
    else if (cmd.indexOf("运动") != -1 || cmd.indexOf("姿态") != -1 || cmd.indexOf("角度") != -1 ||
             cmd.indexOf("计步") != -1 || cmd.indexOf("步数") != -1 || cmd.indexOf("活动") != -1) {
        screen = SCREEN_ID_SPORT;
    }
    else if (cmd.indexOf("主页") != -1 || cmd.indexOf("首页") != -1 || cmd.indexOf("返回") != -1 ||
             cmd.indexOf("表盘") != -1 || cmd.indexOf("时钟") != -1) {
        screen = SCREEN_ID_MAIN;
    }
    else if (cmd.indexOf("你好") != -1) {
        // 打招呼，仅回应一声
    }
    else {
        Serial.println("[Voice] 未匹配到已知指令");
        return;
    }

    // 切屏交给 UI 任务执行（LVGL 只能在 Task_Parc 上下文里用）
    if (screen >= 0) Parc_RequestScreen(screen);
    playAudio(audio_haode, AUDIO_HAODE_SIZE);      // “好的”
}

/* ============================ 十一、初始化 ============================ */
void INMP441_Init(void)
{
    rawBuf  = (int32_t*)ps_malloc(BLOCK_SAMPLES * sizeof(int32_t));
    s16Buf  = (int16_t*)ps_malloc(BLOCK_SAMPLES * sizeof(int16_t));
    pcmBuf  = (int16_t*)ps_malloc(UTTER_MAX_SAMPLES * sizeof(int16_t));
    jsonBuf = (char*)   ps_malloc(JSON_BUF_SIZE);

    if (!rawBuf || !s16Buf || !pcmBuf || !jsonBuf) {
        Serial.println("[Voice] PSRAM 缓冲区分配失败，语音功能停用");
        return;
    }
    Serial.printf("[Voice] PSRAM 就绪：录音 %d B / JSON %d B\n",
                  (int)(UTTER_MAX_SAMPLES * 2), (int)JSON_BUF_SIZE);

    /* token 不在这里取 —— 这一步是 HTTPS 请求，未联网时最长要卡 9 秒，
     * 而本函数跑在 Multitask_Init() 里、FreeRTOS_Start() 之前，
     * 卡住就等于整机上电后白屏等待。改由 Task_WiFi 联网成功后回调
     * INMP441_NetworkReady() 完成。 */
    voiceI2SInit();
    voiceReady = true;
    Serial.println("[Voice] 初始化完成（VAD 触发式整段识别，token 待联网后获取）");
}

/* 由 Task_WiFi 在 Wi-Fi 连上拿到 IP 后调用。
 * voiceRecognize() 内部识别失败时也会自行重试一次 token，这里是启动时的第一次。 */
void INMP441_NetworkReady(void)
{
    if (!voiceReady) return;
    if (baiduToken.length() == 0) {
        Serial.println("[Voice] 网络就绪，开始获取百度 token");
        voiceTokenRefresh();
    }
}

/* ============================================================================
 * 十二、任务
 * ----------------------------------------------------------------------------
 * 平时安静地听，只有真的有人说话才把一整句话送云端。没有任何输入时，
 * 全程零网络请求。
 * ==========================================================================*/
void Task_INMP441(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(300));
    if (!voiceReady) {
        Serial.println("[Voice] 模块未就绪，任务退出");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        /* 未联网时不录音也不识别：省得每次说话都白跑一趟 HTTPS，
         * 顺带免掉 voiceRecognize() 内部失败重试 token 带来的长时间卡顿。 */
        if (!wifi_is_connected()) {
            vState = 0;
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }

        // 语音任务一轮可能持续几秒（整句录音 + 云端往返），「循环周期」
        // 这个概念对它不适用，名义周期记 0，体检表里只关心它的栈余量。
        TaskMon_Tick(TM_VOICE, 0);

        /* ---------- 阶段 1：静默等待唤醒词（不产生网络流量） ---------- */
        vState = 0;
        int n = voiceRecord(pcmBuf, WAKE_MAX_SAMPLES, 0, TAIL_SIL_WAKE_MS);
        if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        vState = 2;
        String heard = voiceRecognize(pcmBuf, n);
        if (heard.length()) lastHeardText = heard;
        if (heard.length() == 0 || !voiceIsWakeWord(heard)) continue;

        /* ---------- 唤醒成功 ---------- */
        Serial.println("[Voice] 检测到唤醒词");
        playAudio(audio_wozai, AUDIO_WOZAI_SIZE);      // “我在”

        /* ---------- 阶段 2：等一条指令 ---------- */
        vState = 1;
        int m = voiceRecord(pcmBuf, UTTER_MAX_SAMPLES, CMD_WAIT_MS, TAIL_SIL_CMD_MS);
        if (m > 0) {
            vState = 2;
            String cmd = voiceRecognize(pcmBuf, m);
            if (cmd.length()) {
                lastHeardText = cmd;
                voiceExecuteCommand(cmd);
            }
        } else {
            Serial.println("[Voice] 等待指令超时，回到待唤醒");
        }
        vState = 0;
    }
}
