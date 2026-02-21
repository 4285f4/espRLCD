#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include "time.h"

// 引入驱动、字体和 RTC 库
#include "display_bsp.h"
#include "font.h"             // DSEG7 84 (大)
#include "orbfont.h"          // Orbitron 22 (中)
#include "secfont.h"          // DSEG7 36 (小)
#include "PCF85063A-SOLDERED.h" // RTC 库

// ================= 1. 全局变量 =================
bool isConfigMode = false;
bool shouldSaveConfig = false;
int lastSyncDate = -1; // 记录上次自动同步的日期

// 配置参数
char ntpServer[40] = "ntp.aliyun.com";
bool showSeconds = true;

// WiFiManager 全局对象及参数指针
WiFiManager wm;
WiFiManagerParameter *p_ntp;
WiFiManagerParameter *p_sec;
char secBuf[5];

// 颜色定义
#define C_BLACK 1
#define C_WHITE 0

// SHTC3 地址
static const uint8_t SHTC3_ADDR = 0x70;

// ================= 2. 硬件定义 =================
#define PIN_CS    12
#define PIN_SCK   11
#define PIN_MOSI  5
#define PIN_DE    40
#define PIN_DIS   41
#define PIN_SDA   13
#define PIN_SCL   14

#define BUTTON_PIN 18  // 按键
#define BAT_ADC_PIN 4  // 电池 ADC

static const int W = 400;
static const int H = 300;

// ================= 3. 对象实例化 =================
DisplayPort RlcdPort(PIN_CS, PIN_SCK, PIN_MOSI, PIN_DE, PIN_DIS, W, H);
GFXcanvas1 canvas(W, H);
Preferences prefs;
PCF85063A rtc;

// ================= 4. SHTC3 驱动函数 =================

uint8_t crc8(const uint8_t *data, int len) {
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ?
                  (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

bool shtc3_cmd(uint16_t cmd) {
    Wire.beginTransmission(SHTC3_ADDR);
    Wire.write((uint8_t)(cmd >> 8));
    Wire.write((uint8_t)(cmd & 0xFF));
    return Wire.endTransmission() == 0;
}

bool shtc3_read(float &tempC, float &rh) {
    if (!shtc3_cmd(0x3517)) return false; 
    delay(1);
    if (!shtc3_cmd(0x7866)) return false; 
    delay(20);

    Wire.requestFrom((int)SHTC3_ADDR, 6);
    if (Wire.available() != 6) return false;

    uint8_t d[6];
    for (int i = 0; i < 6; i++) d[i] = Wire.read();

    if (crc8(&d[0], 2) != d[2]) return false;
    if (crc8(&d[3], 2) != d[5]) return false;

    uint16_t tRaw  = (uint16_t)d[0] << 8 | d[1];
    uint16_t rhRaw = (uint16_t)d[3] << 8 | d[4];

    tempC = -45.0f + 175.0f * (float)tRaw / 65535.0f;
    rh    = 100.0f * (float)rhRaw / 65535.0f;

    shtc3_cmd(0xB098); 
    return true;
}

// ================= 5. 辅助函数 =================

void saveConfigCallback() {
    shouldSaveConfig = true;
}

int getBatteryPercent() {
    uint32_t raw = analogRead(BAT_ADC_PIN);
    float v_bat = (raw / 4095.0f) * 3.3f * 3.0f * 1.079f;
    int pct = (int)((v_bat - 3.3f) / (4.15f - 3.3f) * 100);
    return constrain(pct, 0, 100);
}

void pushCanvasToRLCD() {
    uint8_t *buf = canvas.getBuffer();
    const int bytesPerRow = (W + 7) / 8;
    RlcdPort.RLCD_ColorClear(0xFF);
    
    for (int y = 0; y < H; y++) {
        uint8_t *row = buf + y * bytesPerRow;
        for (int bx = 0; bx < bytesPerRow; bx++) {
            uint8_t v = row[bx];
            if (v == 0) continue; 
            int x0 = bx * 8;
            for (int bit = 0; bit < 8; bit++) {
                if ((v & (0x80 >> bit))) {
                    RlcdPort.RLCD_SetPixel(x0 + bit, y, 0x00);
                }
            }
        }
    }
    RlcdPort.RLCD_Display();
}

void showMessage(const char* msg) {
    canvas.fillScreen(C_WHITE);
    canvas.setFont(NULL); 
    canvas.setTextSize(3); 
    int16_t x1, y1; uint16_t w, h;
    canvas.getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
    canvas.setCursor((W - w) / 2 - x1, 150 - h/2);
    canvas.print(msg);
    pushCanvasToRLCD();
}

void syncTime() {
    showMessage("Syncing...");
    
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500); retries++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        configTime(8 * 3600, 0, ntpServer);
        struct tm t;
        int wait = 0;
        while (!getLocalTime(&t) && wait < 40) {
            delay(100);
            wait++;
        }
        
        if (t.tm_year > 120) { 
            rtc.setTime(t.tm_hour, t.tm_min, t.tm_sec);
            rtc.setDate(t.tm_wday, t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
            showMessage("Success!");
            lastSyncDate = t.tm_mday;
            delay(1000);
        } else {
            showMessage("NTP Error");
            delay(1500);
        }
    } else {
        showMessage("WiFi Error");
        delay(1500);
    }
    
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

// ================= 核心：表盘绘制 =================
void drawWatchFace() {
    static float lastTemp = 0.0; 
    float t_val, h_val;
    if (shtc3_read(t_val, h_val)) {
        lastTemp = t_val;
    }

    canvas.fillScreen(C_WHITE);
    canvas.setTextSize(1);
    
    struct tm t;
    t.tm_hour = rtc.getHour();
    t.tm_min  = rtc.getMinute();
    t.tm_sec  = rtc.getSecond();
    t.tm_mday = rtc.getDay();
    t.tm_wday = rtc.getWeekday();
    t.tm_mon  = rtc.getMonth() - 1; 
    t.tm_year = rtc.getYear() - 1900;

    // 框架
    canvas.fillRect(0, 90, 400, 3, C_BLACK);
    canvas.fillRect(0, 210, 400, 3, C_BLACK); 
    canvas.fillRect(199, 0, 3, 90, C_BLACK);
    canvas.fillRect(199, 210, 3, 90, C_BLACK);  

    // 四角布局
    int16_t x1, y1; uint16_t w, h;
    char buf[40];
    int pX = 15; 
    int topLblY = 64; int topValY = 56; 
    int botLblY = 222; int botValY = 266;

    auto drawLabel = [&](int x, int y, const char* label, bool alignRight) {
        canvas.setFont(NULL);
        canvas.setTextSize(2); 
        canvas.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
        int finalX = alignRight ? (W - w - pX) : x;
        canvas.setCursor(finalX, y);
        canvas.print(label);
    };

    auto drawValue = [&](int x, int y, const char* val, bool alignRight) {
        canvas.setFont(&Orbitron_Medium_22);
        canvas.setTextSize(1);
        canvas.getTextBounds(val, 0, 0, &x1, &y1, &w, &h);
        int finalX = alignRight ? (W - w - pX) : x;
        canvas.setCursor(finalX, y);
        canvas.print(val);
    };

    char weekBuf[15];
    strftime(weekBuf, 15, "%A", &t);
    strupr(weekBuf);
    drawValue(pX, topValY, weekBuf, false);
    drawLabel(pX, topLblY, "WEEK", false);
    
    sprintf(buf, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    drawValue(0, topValY, buf, true); 
    drawLabel(0, topLblY, "DATE", true);
    
    drawLabel(pX, botLblY, "TEMP", false);
    sprintf(buf, "%.1f C", lastTemp);
    drawValue(pX, botValY, buf, false);

    drawLabel(0, botLblY, "POWER", true);
    int bat = getBatteryPercent();
    sprintf(buf, "%d%%", bat);
    drawValue(0, botValY, buf, true);

    // 时间显示
    if (showSeconds) {
        int secBaseY = 185;
        char timeStr[15];
        sprintf(timeStr, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
        canvas.setFont(&DSEG7_Classic_Bold_36);
        canvas.setTextSize(2); 
        canvas.getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);
        int timeX = (W - w) / 2 - x1; 
        canvas.setCursor(timeX, secBaseY); 
        canvas.print(timeStr);
    } else {
        int noSecBaseY = 195;
        canvas.setFont(&DSEG7_Classic_Bold_84);
        canvas.setTextSize(1);
        
        char colon[] = ":";
        canvas.getTextBounds(colon, 0, 0, &x1, &y1, &w, &h);
        int colonX = (W - w) / 2 - x1; 
        canvas.setCursor(colonX, noSecBaseY);
        canvas.print(colon);

        char hourStr[5];
        sprintf(hourStr, "%02d", t.tm_hour);
        canvas.getTextBounds(hourStr, 0, 0, &x1, &y1, &w, &h);
        canvas.setCursor(180 - w - x1, noSecBaseY);
        canvas.print(hourStr);

        char minStr[5];
        sprintf(minStr, "%02d", t.tm_min);
        canvas.getTextBounds(minStr, 0, 0, &x1, &y1, &w, &h);
        canvas.setCursor(220 - x1, noSecBaseY);
        canvas.print(minStr);
    }

    pushCanvasToRLCD();
}

// ================= Config 模式逻辑 =================

void startConfigMode() {
    isConfigMode = true;
    shouldSaveConfig = false; 
    
    showMessage("AP: RLCD-Clock"); 
    
    p_ntp->setValue(ntpServer, 40);
    strcpy(secBuf, showSeconds ? "1" : "0");
    p_sec->setValue(secBuf, 4);
    
    wm.setConfigPortalBlocking(false);
    wm.startConfigPortal("RLCD-Clock");
}

void exitConfigMode() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    
    isConfigMode = false;
    showMessage("Exit AP Mode");
    delay(1000);
    drawWatchFace();
    delay(50);
}

void click() { 
    if(!isConfigMode) syncTime(); 
}

void longPress() { 
    if (!isConfigMode) {
        startConfigMode();
    } else {
        exitConfigMode();
    }
}

// ================= 替代 OneButton 的自定义按键逻辑 =================
void handleButton() {
    delay(20); // 软件防抖
    if (digitalRead(BUTTON_PIN) != LOW) return;

    unsigned long pressTime = millis();
    bool isLongPress = false;

    while (digitalRead(BUTTON_PIN) == LOW) {
        if (millis() - pressTime > 800) { 
            isLongPress = true;
            break;
        }
        delay(10);
    }

    if (isLongPress) {
        longPress(); 
        while(digitalRead(BUTTON_PIN) == LOW) delay(10); 
    } else {
        click();     
        while(digitalRead(BUTTON_PIN) == LOW) delay(10); 
    }
}

// ================= Arduino Setup & Loop =================
void setup() {
    Serial.begin(115200);
    delay(500);
    Wire.begin(PIN_SDA, PIN_SCL);
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    gpio_wakeup_enable((gpio_num_t)BUTTON_PIN, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    RlcdPort.RLCD_Init();
    RlcdPort.RLCD_ColorClear(0xFF);
    RlcdPort.RLCD_Display();
    delay(50); // 确保初始化清屏画面显示完整
    
    rtc.begin();
    analogReadResolution(12);

    prefs.begin("watch", true);
    if(prefs.isKey("ntp")) {
        prefs.getString("ntp", "ntp.aliyun.com").toCharArray(ntpServer, 40);
        showSeconds = prefs.getBool("sec", true);
    }
    prefs.end();

    p_ntp = new WiFiManagerParameter("ntp", "NTP Server", ntpServer, 40);
    strcpy(secBuf, showSeconds ? "1" : "0");
    p_sec = new WiFiManagerParameter("sec", "Show Seconds (1/0)", secBuf, 4);

    wm.addParameter(p_ntp);
    wm.addParameter(p_sec);
    wm.setSaveConfigCallback(saveConfigCallback);
    wm.setBreakAfterConfig(true); 

    setCpuFrequencyMhz(80); 
    
    // 【问题一修复】移除开机时的 drawWatchFace()，避免闪现旧时间界面
    
    // 开机强制执行一次 NTP 同步
    syncTime(); 
}

void loop() {
    // 1. 优先处理按键唤醒或正常运行时的按键按下
    if (digitalRead(BUTTON_PIN) == LOW) {
        handleButton();
        return; 
    }

    // 2. 配网模式处理
    if (isConfigMode) {
        wm.process();
        if (shouldSaveConfig) {
            strncpy(ntpServer, p_ntp->getValue(), 40);
            const char* secVal = p_sec->getValue();
            showSeconds = (secVal[0] != '0');

            prefs.begin("watch", false);
            prefs.putString("ntp", ntpServer);
            prefs.putBool("sec", showSeconds);
            prefs.end();

            showMessage("Saved!");
            delay(1000);
            isConfigMode = false;
            WiFi.mode(WIFI_OFF); 
            syncTime(); 
            shouldSaveConfig = false;
        }
        delay(10);
        return; 
    }

    // 3. 正常时钟模式
    uint64_t start_us = esp_timer_get_time();

    int currentDay = rtc.getDay();
    int currentHour = rtc.getHour();
    
    if (currentHour == 0 && currentDay != lastSyncDate) {
        syncTime();
    }

    // 绘制屏幕
    drawWatchFace();

    // 等待 SPI DMA 传输完成，防止休眠切断时钟
    delay(50);

    // 4. 计算休眠时长并进入 Light Sleep
    uint64_t end_us = esp_timer_get_time();
    uint64_t cost_us = end_us - start_us; 

    uint64_t sleep_us = 0;
    if (showSeconds) {
        sleep_us = 1000000ULL; 
        if (sleep_us > cost_us) {
            sleep_us -= cost_us; // 显秒时由于每秒都刷，必须减去耗时防止秒级漂移
        } else {
            sleep_us = 10000; 
        }
    } else {
        int sec = rtc.getSecond();
        sleep_us = (60 - sec) * 1000000ULL; 
        // 【问题二修复】不显秒时，严禁减去 cost_us！
        // 利用 RTC 读取的秒数形成负反馈闭环：强迫设备每次休眠唤醒都在真实的 "00~01 秒" 之间。
        // 这样彻底杜绝了因微秒级漂移导致提前在 "59.9秒" 醒来而画错分钟的 Bug。
    }

    esp_sleep_enable_timer_wakeup(sleep_us);
    esp_light_sleep_start();
}