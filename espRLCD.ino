#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include "time.h"
#include <sys/time.h>         // 用于 settimeofday 和 gettimeofday
#include "driver/gpio.h" 
#include "driver/rtc_io.h"    // 用于 RTC GPIO 保持和上拉

// 引入驱动、字体和 RTC 库
#include "display_bsp.h"
#include "font.h"             // DSEG7 84 (大)
#include "orbfont.h"          // Orbitron 22 (中)
#include "secfont.h"          // DSEG7 36 (小)
#include "PCF85063A-SOLDERED.h" // RTC 库

// ================= 1. RTC 掉电保护内存 (深度睡眠专用) =================
RTC_DATA_ATTR int rtc_lastSyncDate = -1;
RTC_DATA_ATTR bool rtc_isFirstBoot = true;

// ================= 2. 全局变量 =================
bool isConfigMode = false;
bool shouldSaveConfig = false;
char ntpServer[40] = "ntp.aliyun.com";
bool showSeconds = true;
char customMemo[10] = "WORK HARD";

WiFiManager wm;
WiFiManagerParameter *p_ntp;
WiFiManagerParameter *p_sec;
WiFiManagerParameter *p_memo; 
char secBuf[5];

#define C_BLACK 1
#define C_WHITE 0

// ================= 3. 硬件定义 =================
#define PIN_CS    12
#define PIN_SCK   11
#define PIN_MOSI  5
#define PIN_DE    40
#define PIN_DIS   41
#define PIN_SDA   13
#define PIN_SCL   14

#define BUTTON_PIN 18  
#define BAT_ADC_PIN 4  

static const int W = 400;
static const int H = 300;

// ================= 4. 对象实例化 =================
DisplayPort RlcdPort(PIN_CS, PIN_SCK, PIN_MOSI, PIN_DE, PIN_DIS, W, H);
GFXcanvas1 canvas(W, H);
Preferences prefs;
PCF85063A rtc;

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
        struct timeval tv = {0, 0};
        settimeofday(&tv, NULL);

        configTime(8 * 3600, 0, ntpServer);
        
        struct tm t;
        int wait = 0;
        
        while (wait < 40) {
            getLocalTime(&t, 10); 
            if (t.tm_year > 120) {
                break; 
            }
            delay(100);
            wait++;
        }
        
        if (t.tm_year > 120) { 
            // 【终极修复 1：毫秒级精准对齐】
            // 死循环等待内部时间刚好跨入下一秒的 000 毫秒，再精准扣动扳机写入 RTC
            gettimeofday(&tv, NULL);
            time_t current_sec = tv.tv_sec;
            while (tv.tv_sec == current_sec) {
                gettimeofday(&tv, NULL);
                delay(1); 
            }
            
            // 此时刚好跨入新的一秒（0毫秒），获取最新的整秒时间并立刻写入 RTC
            getLocalTime(&t, 0); 
            rtc.setTime(t.tm_hour, t.tm_min, t.tm_sec);
            rtc.setDate(t.tm_wday, t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);
            
            showMessage("Success!");
            rtc_lastSyncDate = t.tm_mday;
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

    canvas.fillRect(0, 90, 400, 3, C_BLACK);
    canvas.fillRect(0, 210, 400, 3, C_BLACK); 
    canvas.fillRect(199, 0, 3, 90, C_BLACK);
    canvas.fillRect(199, 210, 3, 90, C_BLACK);  

    int16_t x1, y1;
    uint16_t w, h; char buf[40];
    int pX = 15; int topLblY = 64; int topValY = 56;
    int botLblY = 222; int botValY = 266;

    auto drawLabel = [&](int x, int y, const char* label, bool alignRight) {
        canvas.setFont(NULL);
        canvas.setTextSize(2); 
        canvas.getTextBounds(label, 0, 0, &x1, &y1, &w, &h);
        int finalX = alignRight ? (W - w - pX) : x;
        canvas.setCursor(finalX, y); canvas.print(label);
    };

    auto drawValue = [&](int x, int y, const char* val, bool alignRight) {
        canvas.setFont(&Orbitron_Medium_22);
        canvas.setTextSize(1);
        canvas.getTextBounds(val, 0, 0, &x1, &y1, &w, &h);
        int finalX = alignRight ? (W - w - pX) : x;
        canvas.setCursor(finalX, y); canvas.print(val);
    };

    char weekBuf[15]; strftime(weekBuf, 15, "%A", &t); strupr(weekBuf);
    drawValue(pX, topValY, weekBuf, false);
    drawLabel(pX, topLblY, "WEEK", false);
    
    sprintf(buf, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    drawValue(0, topValY, buf, true); drawLabel(0, topLblY, "DATE", true);
    
    char upperMemo[10];
    strncpy(upperMemo, customMemo, 9);
    upperMemo[9] = '\0'; 
    strupr(upperMemo);
    drawLabel(pX, botLblY, "MEMO", false);
    drawValue(pX, botValY, upperMemo, false);

    drawLabel(0, botLblY, "POWER", true);
    int bat = getBatteryPercent(); sprintf(buf, "%d%%", bat);
    drawValue(0, botValY, buf, true);
    
    if (showSeconds) {
        int secBaseY = 185;
        char timeStr[15];
        sprintf(timeStr, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
        canvas.setFont(&DSEG7_Classic_Bold_36); canvas.setTextSize(2); 
        canvas.getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);
        int timeX = (W - w) / 2 - x1; canvas.setCursor(timeX, secBaseY); canvas.print(timeStr);
    } else {
        int noSecBaseY = 195; canvas.setFont(&DSEG7_Classic_Bold_84); canvas.setTextSize(1);
        char colon[] = ":";
        canvas.getTextBounds(colon, 0, 0, &x1, &y1, &w, &h);
        int colonX = (W - w) / 2 - x1; canvas.setCursor(colonX, noSecBaseY); canvas.print(colon);
        char hourStr[5]; sprintf(hourStr, "%02d", t.tm_hour);
        canvas.getTextBounds(hourStr, 0, 0, &x1, &y1, &w, &h); canvas.setCursor(180 - w - x1, noSecBaseY); canvas.print(hourStr);
        char minStr[5]; sprintf(minStr, "%02d", t.tm_min);
        canvas.getTextBounds(minStr, 0, 0, &x1, &y1, &w, &h); canvas.setCursor(220 - x1, noSecBaseY); canvas.print(minStr);
    }

    pushCanvasToRLCD();
}

// ================= Config 模式与按键逻辑 =================

void startConfigMode() {
    isConfigMode = true;
    shouldSaveConfig = false; 
    showMessage("AP: RLCD-Clock"); 
    
    p_ntp->setValue(ntpServer, 40); 
    strcpy(secBuf, showSeconds ? "1" : "0"); p_sec->setValue(secBuf, 4);
    p_memo->setValue(customMemo, 10);

    wm.setConfigPortalBlocking(false); 
    wm.startConfigPortal("RLCD-Clock");
}

void exitConfigMode() {
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
    isConfigMode = false;
    showMessage("Exit AP Mode"); delay(1000); drawWatchFace(); delay(50);
}

void click() { if(!isConfigMode) syncTime(); }

void longPress() { if (!isConfigMode) startConfigMode(); else exitConfigMode();
}

void handleButton() {
    delay(20); if (digitalRead(BUTTON_PIN) != LOW) return;
    unsigned long pressTime = millis();
    bool isLongPress = false;
    while (digitalRead(BUTTON_PIN) == LOW) {
        if (millis() - pressTime > 800) { isLongPress = true;
        break; }
        delay(10);
    }
    if (isLongPress) { longPress();
    while(digitalRead(BUTTON_PIN) == LOW) delay(10); } 
    else { click(); while(digitalRead(BUTTON_PIN) == LOW) delay(10);
    }
}

// ================= Arduino Setup & Loop =================
void setup() {
    gpio_hold_dis((gpio_num_t)PIN_CS);
    gpio_hold_dis((gpio_num_t)PIN_SCK);
    gpio_hold_dis((gpio_num_t)PIN_MOSI);
    gpio_hold_dis((gpio_num_t)PIN_DE);
    gpio_hold_dis((gpio_num_t)PIN_DIS);
    gpio_deep_sleep_hold_dis();

    Serial.begin(115200);
    Wire.begin(PIN_SDA, PIN_SCL);
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    gpio_wakeup_enable((gpio_num_t)BUTTON_PIN, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    rtc.begin();
    analogReadResolution(12);

    prefs.begin("watch", true);
    if(prefs.isKey("ntp")) {
        prefs.getString("ntp", "ntp.aliyun.com").toCharArray(ntpServer, 40);
        showSeconds = prefs.getBool("sec", true);
        if(prefs.isKey("memo")) {
            prefs.getString("memo", "WORK HARD").toCharArray(customMemo, 10);
        }
    }
    prefs.end();

    p_ntp = new WiFiManagerParameter("ntp", "NTP Server", ntpServer, 40);
    strcpy(secBuf, showSeconds ? "1" : "0");
    p_sec = new WiFiManagerParameter("sec", "Show Seconds (1/0)", secBuf, 4);
    p_memo = new WiFiManagerParameter("memo", "Custom Memo (Max 9 chars)", customMemo, 10);
    wm.addParameter(p_ntp); 
    wm.addParameter(p_sec);
    wm.addParameter(p_memo); 
    wm.setSaveConfigCallback(saveConfigCallback);
    wm.setBreakAfterConfig(true); 

    setCpuFrequencyMhz(80); 

    bool handled_in_setup = false;
    delay(20);
    if (digitalRead(BUTTON_PIN) == LOW) {
        RlcdPort.RLCD_Init();
        RlcdPort.RLCD_ColorClear(0xFF);
        RlcdPort.RLCD_Display();
        delay(50); 
        
        unsigned long pressTime = millis();
        bool isLongPress = false;
        while (digitalRead(BUTTON_PIN) == LOW) {
            if (millis() - pressTime > 800) { isLongPress = true;
            break; }
            delay(10);
        }

        if (isLongPress) {
            longPress();
            while(digitalRead(BUTTON_PIN) == LOW) delay(10);
        } else {
            click();
            while(digitalRead(BUTTON_PIN) == LOW) delay(10);
        }
        
        handled_in_setup = true;
        rtc_isFirstBoot = false; 
    }

    if (!handled_in_setup) {
        esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
        bool isDeepSleepWake = (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) && (!showSeconds);

        if (rtc_isFirstBoot || !isDeepSleepWake) {
            RlcdPort.RLCD_Init();
            RlcdPort.RLCD_ColorClear(0xFF);
            RlcdPort.RLCD_Display();
            delay(50); 
            syncTime(); 
            rtc_isFirstBoot = false;
        } else {
            if (rtc.getHour() == 0 && rtc.getDay() != rtc_lastSyncDate) {
                syncTime();
            }
        }
    }
}

void loop() {
    if (digitalRead(BUTTON_PIN) == LOW) {
        handleButton();
        return; 
    }

    if (isConfigMode) {
        wm.process();
        if (shouldSaveConfig) {
            strncpy(ntpServer, p_ntp->getValue(), 40);
            showSeconds = (p_sec->getValue()[0] != '0');
            strncpy(customMemo, p_memo->getValue(), 9);
            customMemo[9] = '\0'; 

            prefs.begin("watch", false); 
            prefs.putString("ntp", ntpServer);
            prefs.putBool("sec", showSeconds); 
            prefs.putString("memo", customMemo); 
            prefs.end();

            showMessage("Saved!"); delay(1000);
            isConfigMode = false; WiFi.mode(WIFI_OFF); syncTime(); shouldSaveConfig = false;
        }
        delay(10);
        return; 
    }

    // 【终极修复 2：消除屏幕刷新的相位滞后】
    static int last_sec = -1;
    if (showSeconds) {
        // 提前唤醒，死等 RTC 硬件秒数翻篇的瞬间。一旦翻篇立刻刷屏，视觉上做到 0 延迟
        int current_sec = rtc.getSecond();
        while (current_sec == last_sec) {
            delay(5);
            current_sec = rtc.getSecond();
        }
        last_sec = current_sec;
    }

    uint64_t start_us = esp_timer_get_time();
    drawWatchFace();
    delay(50);
    uint64_t cost_us = esp_timer_get_time() - start_us; 

    if (showSeconds) {
        // 既然我们在 loop 开头会精准抓取翻篇瞬间，这里只要睡得比 1 秒少一点即可（比如固定睡 850ms）
        // 留下充足的 150ms 缓冲交给下一次 loop 开头去死等
        esp_sleep_enable_timer_wakeup(850000ULL); 
        esp_light_sleep_start();
    } else {
        int sec = rtc.getSecond();
        uint64_t target_sleep_us = (60 - sec) * 1000000ULL; 
        if (target_sleep_us == 0) target_sleep_us = 60000000ULL;
        
        uint64_t sleep_us = target_sleep_us;
        if (sleep_us > cost_us) {
            sleep_us -= cost_us;
        } else {
            sleep_us = 10000;
        }
        
        esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0); 
        rtc_gpio_pullup_en((gpio_num_t)BUTTON_PIN);
        
        gpio_hold_en((gpio_num_t)PIN_CS);
        gpio_hold_en((gpio_num_t)PIN_SCK);
        gpio_hold_en((gpio_num_t)PIN_MOSI);
        gpio_hold_en((gpio_num_t)PIN_DE);
        gpio_hold_en((gpio_num_t)PIN_DIS);
        gpio_deep_sleep_hold_en(); 

        esp_sleep_enable_timer_wakeup(sleep_us);
        esp_deep_sleep_start(); 
    }
}