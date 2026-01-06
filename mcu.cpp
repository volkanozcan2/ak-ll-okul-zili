/*
  Akıllı Okul Zili (ESP32 + DFPlayer Mini + RTC DS3231)

  Özellikler:
  - RTC (DS3231) üzerinden offline çalışır.
  - Wi-Fi varsa NTP ile saati günceller ve RTC'ye yazar.
  - Zil çizelgesine göre otomatik MP3 çalar.
  - Web API ile manuel müzik/anons çalma, durdurma, ses ayarı ve durum/schedule görüntüleme.

  Web API:
  - GET  /status              -> JSON durum
  - GET  /play?track=1         -> Track çal (001.mp3 -> 1)
  - GET  /stop                -> Durdur
  - GET  /volume?v=20          -> Ses (0..30)
  - GET  /schedule            -> Zil çizelgesi JSON

  Donanım Bağlantıları (Öneri):
  RTC DS3231 (I2C):
    SDA -> GPIO 21
    SCL -> GPIO 22
    VCC -> 3.3V (modüle göre 5V da olabilir)
    GND -> GND

  DFPlayer Mini (UART):
    DF TX -> ESP32 RX2 (GPIO 16)
    DF RX -> ESP32 TX2 (GPIO 17)  (DF RX hattına 1K seri direnç önerilir)
    VCC  -> 5V
    GND  -> GND
*/

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <NTPClient.h>

#include <Wire.h>
#include "RTClib.h"

#include <DFRobotDFPlayerMini.h>

// ----------------------- Wi-Fi Ayarları -----------------------
const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";

// Türkiye saati: UTC+3
static const long UTC_OFFSET_SECONDS = 3 * 3600;

// ----------------------- UART (DFPlayer) ----------------------
HardwareSerial DFSerial(2);  // ESP32 UART2
static const int DF_RX = 16; // ESP32 RX2 (DF TX)
static const int DF_TX = 17; // ESP32 TX2 (DF RX)

// ----------------------- Global Nesneler ----------------------
WebServer server(80);

WiFiUDP ntpUDP;
NTPClient ntp(ntpUDP, "pool.ntp.org", UTC_OFFSET_SECONDS, 60 * 60 * 1000); // 1 saatte bir

RTC_DS3231 rtc;
DFRobotDFPlayerMini df;

// ----------------------- Zil Çizelgesi ------------------------
// dow: ISO 1=Mon ... 7=Sun
// track: DFPlayer parça no (001.mp3 -> 1)
struct BellEvent
{
    uint8_t dowFrom;   // 1..7
    uint8_t dowTo;     // 1..7
    uint8_t hour;      // 0..23
    uint8_t minute;    // 0..59
    uint16_t track;    // 1..3000
    const char *label; // açıklama
};

// Örnek program (kendi okul çizelgene göre değiştir)
BellEvent scheduleList[] = {
    {1, 5, 8, 30, 1, "Ders Baslangic"},
    {1, 5, 9, 10, 2, "Teneffus"},
    {1, 5, 9, 20, 1, "Ders"},
    {1, 5, 12, 00, 3, "Istiklal Marsi"},
    {1, 5, 16, 00, 4, "Cikis"}};
const size_t SCHEDULE_COUNT = sizeof(scheduleList) / sizeof(scheduleList[0]);

// Aynı dakika içinde tekrar çalmayı engelleme
int lastTriggeredYear = -1, lastTriggeredMonth = -1, lastTriggeredDay = -1;
int lastTriggeredHour = -1, lastTriggeredMinute = -1;
uint16_t lastTriggeredTrack = 0;

// Ses seviyesi (0..30)
int currentVolume = 20;

// Manuel mod (idareci çalma başlatınca otomatik tetiklemeyi kilitlemek için)
bool manualLock = false;
uint32_t manualLockUntilMs = 0;
static const uint32_t MANUAL_LOCK_MS = 3UL * 60UL * 1000UL; // 3 dakika

// ----------------------- Yardımcı Fonksiyonlar ----------------
static uint8_t rtcDowToIsoMon1(DateTime now)
{
    // RTClib dayOfTheWeek(): 0=Sun..6=Sat
    // ISO: 1=Mon..7=Sun
    uint8_t d = now.dayOfTheWeek();
    if (d == 0)
        return 7;
    return d;
}

static bool dowInRange(uint8_t isoDow, uint8_t from, uint8_t to)
{
    // Basit aralık (Mon-Fri). Wrap-around ihtiyacı yoksa yeterli.
    return (isoDow >= from && isoDow <= to);
}

void setManualLock()
{
    manualLock = true;
    manualLockUntilMs = millis() + MANUAL_LOCK_MS;
}

void updateManualLock()
{
    if (manualLock && (int32_t)(millis() - manualLockUntilMs) >= 0)
    {
        manualLock = false;
    }
}

void playTrack(uint16_t track)
{
    df.volume(currentVolume);
    df.play(track);
}

void stopPlayback()
{
    df.stop();
}

bool connectWifiQuick(uint32_t timeoutMs = 6000)
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs)
    {
        delay(150);
    }
    return WiFi.status() == WL_CONNECTED;
}

bool syncRtcFromNtp()
{
    if (WiFi.status() != WL_CONNECTED)
        return false;

    ntp.begin();
    if (!ntp.update())
        ntp.forceUpdate();

    unsigned long epoch = ntp.getEpochTime();
    if (epoch < 1700000000UL)
        return false; // kaba doğrulama

    rtc.adjust(DateTime((uint32_t)epoch));
    return true;
}

// ----------------------- Web Handlers -------------------------
void handleRoot()
{
    server.send(200, "text/plain; charset=utf-8",
                "Akilli Okul Zili\n"
                "GET /status\n"
                "GET /play?track=1\n"
                "GET /stop\n"
                "GET /volume?v=20\n"
                "GET /schedule\n");
}

void handleStatus()
{
    DateTime now = rtc.now();
    char ts[32];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
             now.year(), now.month(), now.day(),
             now.hour(), now.minute(), now.second());

    String s = "{";
    s += "\"wifi\":\"" + String((WiFi.status() == WL_CONNECTED) ? "connected" : "disconnected") + "\",";
    s += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    s += "\"rtc\":\"" + String(ts) + "\",";
    s += "\"volume\":" + String(currentVolume) + ",";
    s += "\"manualLock\":" + String(manualLock ? "true" : "false");
    s += "}";

    server.send(200, "application/json; charset=utf-8", s);
}

void handlePlay()
{
    if (!server.hasArg("track"))
    {
        server.send(400, "text/plain; charset=utf-8", "Missing track");
        return;
    }
    int t = server.arg("track").toInt();
    if (t <= 0 || t > 3000)
    {
        server.send(400, "text/plain; charset=utf-8", "Invalid track");
        return;
    }

    setManualLock(); // manuel çalma başlatınca otomatik tetiklemeyi kilitle
    playTrack((uint16_t)t);

    server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleStop()
{
    stopPlayback();
    server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleVolume()
{
    if (!server.hasArg("v"))
    {
        server.send(400, "text/plain; charset=utf-8", "Missing v");
        return;
    }
    int v = server.arg("v").toInt();
    if (v < 0)
        v = 0;
    if (v > 30)
        v = 30;

    currentVolume = v;
    df.volume(currentVolume);

    server.send(200, "text/plain; charset=utf-8", "OK");
}

void handleSchedule()
{
    String s = "[";
    for (size_t i = 0; i < SCHEDULE_COUNT; i++)
    {
        const auto &e = scheduleList[i];
        s += "{";
        s += "\"dowFrom\":" + String(e.dowFrom) + ",";
        s += "\"dowTo\":" + String(e.dowTo) + ",";
        s += "\"hour\":" + String(e.hour) + ",";
        s += "\"minute\":" + String(e.minute) + ",";
        s += "\"track\":" + String(e.track) + ",";
        s += "\"label\":\"" + String(e.label) + "\"";
        s += "}";
        if (i + 1 < SCHEDULE_COUNT)
            s += ",";
    }
    s += "]";
    server.send(200, "application/json; charset=utf-8", s);
}

// ----------------------- Setup / Loop --------------------------
void setup()
{
    Serial.begin(115200);
    delay(200);

    // RTC (I2C)
    Wire.begin(21, 22);
    if (!rtc.begin())
    {
        Serial.println("RTC not found!");
    }

    // DFPlayer (UART2)
    DFSerial.begin(9600, SERIAL_8N1, DF_RX, DF_TX);
    if (!df.begin(DFSerial))
    {
        Serial.println("DFPlayer init failed!");
    }
    else
    {
        df.volume(currentVolume);
        Serial.println("DFPlayer ready.");
    }

    // Wi-Fi + NTP -> RTC (başarısızsa RTC ile devam)
    if (connectWifiQuick())
    {
        Serial.print("WiFi OK IP: ");
        Serial.println(WiFi.localIP());
        bool ok = syncRtcFromNtp();
        Serial.println(ok ? "RTC synced from NTP." : "NTP sync failed, using RTC.");
    }
    else
    {
        Serial.println("WiFi not connected, using RTC.");
    }

    // Web server
    server.on("/", handleRoot);
    server.on("/status", handleStatus);
    server.on("/play", handlePlay);
    server.on("/stop", handleStop);
    server.on("/volume", handleVolume);
    server.on("/schedule", handleSchedule);
    server.begin();

    Serial.println("HTTP server started.");
}

void loop()
{
    server.handleClient();
    updateManualLock();

    static uint32_t lastTickMs = 0;
    if (millis() - lastTickMs < 250)
        return;
    lastTickMs = millis();

    // Manuel kilit açıksa otomatik tetikleme yapılmaz
    if (manualLock)
        return;

    DateTime now = rtc.now();
    uint8_t isoDow = rtcDowToIsoMon1(now);

    // Dakikanın ilk saniyesinde tetikle
    if (now.second() != 0)
        return;

    for (size_t i = 0; i < SCHEDULE_COUNT; i++)
    {
        const auto &e = scheduleList[i];

        if (!dowInRange(isoDow, e.dowFrom, e.dowTo))
            continue;
        if (now.hour() != e.hour || now.minute() != e.minute)
            continue;

        // Aynı dakika aynı track tekrar çalmasın
        if (now.year() == lastTriggeredYear &&
            now.month() == lastTriggeredMonth &&
            now.day() == lastTriggeredDay &&
            now.hour() == lastTriggeredHour &&
            now.minute() == lastTriggeredMinute &&
            e.track == lastTriggeredTrack)
        {
            continue;
        }

        playTrack(e.track);

        lastTriggeredYear = now.year();
        lastTriggeredMonth = now.month();
        lastTriggeredDay = now.day();
        lastTriggeredHour = now.hour();
        lastTriggeredMinute = now.minute();
        lastTriggeredTrack = e.track;

        Serial.printf("Triggered: %s track=%u at %02d:%02d\n",
                      e.label, e.track, e.hour, e.minute);
    }
}
