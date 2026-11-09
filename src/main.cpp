#include <Arduino.h>
#include <M5Stack.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>

#include "music_builtin.h"

namespace {

constexpr uint8_t kAlarmCount = 5;
constexpr uint8_t kBuiltinMusicSound = 0;
constexpr uint8_t kLedCount = 10;
constexpr uint8_t kLedPin = 15;
constexpr uint8_t kMainModeCount = 7;
constexpr uint8_t kAlarmMenuCount = kAlarmCount + 1;
constexpr uint8_t kWifiMaxScanRows = 6;
constexpr uint8_t kWifiMenuCount = kWifiMaxScanRows + 2;
constexpr uint32_t kHomeRefreshMs = 1000;
constexpr uint32_t kToolRefreshMs = 250;
constexpr uint32_t kAlarmTimeoutMs = 60000;
constexpr uint32_t kNtpRetryMs = 10 * 60 * 1000;
constexpr long kChinaGmtOffsetSeconds = 8 * 3600;

struct Alarm {
  uint8_t hour;
  uint8_t minute;
  uint8_t sound;
  bool enabled;
};

enum class Page {
  Home,
  AlarmList,
  AlarmEdit,
  SoundPreview,
  Stopwatch,
  Countdown,
  Pomodoro,
  WiFiStatus,
  WiFiScan,
};

enum class EditField {
  Hour,
  Minute,
  Preview,
  Enabled,
  Done,
};

const char *kBuiltinMusicName = "Music";

Preferences prefs;
WiFiManager wifiManager;
Adafruit_NeoPixel pixels(kLedCount, kLedPin, NEO_GRB + NEO_KHZ800);

Page page = Page::Home;
EditField editField = EditField::Hour;
Alarm alarms[kAlarmCount] = {
    {7, 0, 0, true},
    {7, 30, 0, false},
    {8, 0, 0, false},
    {12, 30, 0, false},
    {22, 30, 0, false},
};

uint8_t selectedAlarm = 0;
uint8_t selectedMainMode = 0;
uint8_t selectedWifi = 0;
uint8_t wifiScanCount = 0;
bool ringing = false;
int ringingAlarm = -1;
uint32_t ringingStartedMs = 0;
uint32_t lastDrawMs = 0;
uint32_t lastNtpAttemptMs = 0;
bool timeSynced = false;
uint8_t lastAlarmDay = 255;
int lastAlarmMinuteOfDay = -1;
bool needsFullDraw = true;
bool currentFullRedraw = true;
volatile bool builtinMusicPlaying = false;
volatile bool builtinMusicRestartRequested = false;
TaskHandle_t builtinMusicTaskHandle = nullptr;

bool stopwatchRunning = false;
uint32_t stopwatchStartedMs = 0;
uint32_t stopwatchElapsedMs = 0;

uint32_t countdownDurationSec = 5 * 60;
uint32_t countdownRemainingSec = 5 * 60;
bool countdownRunning = false;
uint32_t countdownStartedMs = 0;

bool pomodoroRunning = false;
bool pomodoroWorkPhase = true;
uint32_t pomodoroRemainingSec = 25 * 60;
uint32_t pomodoroStartedMs = 0;
uint8_t pomodoroRound = 1;
bool ledReady = false;
bool rotate180 = false;
uint32_t lastLedMs = 0;
uint8_t ledPhase = 0;
String wifiSsids[kWifiMaxScanRows];
int32_t wifiRssi[kWifiMaxScanRows] = {0};

void markDirty()
{
  needsFullDraw = true;
}

void builtinMusicTask(void *arg)
{
  (void)arg;
  uint32_t index = 0;
  const uint32_t delayUs = 1000000UL / kBuiltinMusicPlaybackRate;

  while (builtinMusicPlaying) {
    if (builtinMusicRestartRequested) {
      index = 0;
      builtinMusicRestartRequested = false;
    }
    const uint8_t sample = pgm_read_byte(&kBuiltinMusic[index]);
    M5.Speaker.write(sample);
    index++;
    if (index >= kBuiltinMusicLength) {
      index = 0;
    }
    delayMicroseconds(delayUs);
  }

  M5.Speaker.mute();
  builtinMusicTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

void ledsOff()
{
  if (!ledReady) {
    return;
  }
  pixels.clear();
  pixels.show();
}

void setAllLeds(uint32_t color)
{
  if (!ledReady) {
    return;
  }
  for (uint8_t i = 0; i < kLedCount; ++i) {
    pixels.setPixelColor(i, color);
  }
  pixels.show();
}

void initLeds()
{
  pixels.begin();
  pixels.setBrightness(48);
  ledReady = true;
  ledsOff();
}

void serviceLedFlash(bool active)
{
  if (!active || !ledReady) {
    if (ledPhase != 0) {
      ledPhase = 0;
      ledsOff();
    }
    return;
  }

  if (millis() - lastLedMs < 120) {
    return;
  }

  lastLedMs = millis();
  ledPhase = (ledPhase + 1) % 6;
  switch (ledPhase) {
    case 0: setAllLeds(pixels.Color(0, 0, 0)); break;
    case 1: setAllLeds(pixels.Color(255, 24, 0)); break;
    case 2: setAllLeds(pixels.Color(255, 180, 0)); break;
    case 3: setAllLeds(pixels.Color(0, 120, 255)); break;
    case 4: setAllLeds(pixels.Color(180, 0, 255)); break;
    case 5: setAllLeds(pixels.Color(255, 255, 255)); break;
  }
}

void startBuiltinMusic()
{
  if (builtinMusicPlaying || builtinMusicTaskHandle != nullptr) {
    return;
  }
  builtinMusicPlaying = true;
  const BaseType_t created = xTaskCreatePinnedToCore(
      builtinMusicTask,
      "builtin_music",
      2048,
      nullptr,
      2,
      &builtinMusicTaskHandle,
      0);
  if (created != pdPASS) {
    builtinMusicPlaying = false;
    builtinMusicTaskHandle = nullptr;
    Serial.println("Music task allocation failed");
  }
}

void stopBuiltinMusic()
{
  if (!builtinMusicPlaying) {
    return;
  }
  builtinMusicRestartRequested = false;
  builtinMusicPlaying = false;
  for (uint8_t i = 0; builtinMusicTaskHandle && i < 20; ++i) {
    delay(5);
  }
}

void saveSettings()
{
  prefs.putUInt("cdsec", countdownDurationSec);
  prefs.putBool("rot180", rotate180);
  for (uint8_t i = 0; i < kAlarmCount; ++i) {
    char key[10];
    snprintf(key, sizeof(key), "h%u", i);
    prefs.putUChar(key, alarms[i].hour);
    snprintf(key, sizeof(key), "m%u", i);
    prefs.putUChar(key, alarms[i].minute);
    snprintf(key, sizeof(key), "s%u", i);
    prefs.putUChar(key, kBuiltinMusicSound);
    snprintf(key, sizeof(key), "e%u", i);
    prefs.putBool(key, alarms[i].enabled);
  }
}

void loadSettings()
{
  countdownDurationSec = prefs.getUInt("cdsec", 5 * 60);
  rotate180 = prefs.getBool("rot180", false);
  countdownDurationSec = constrain(countdownDurationSec, 60UL, 99UL * 60UL);
  countdownRemainingSec = countdownDurationSec;
  for (uint8_t i = 0; i < kAlarmCount; ++i) {
    char key[10];
    snprintf(key, sizeof(key), "h%u", i);
    alarms[i].hour = prefs.getUChar(key, alarms[i].hour);
    snprintf(key, sizeof(key), "m%u", i);
    alarms[i].minute = prefs.getUChar(key, alarms[i].minute);
    alarms[i].sound = kBuiltinMusicSound;
    snprintf(key, sizeof(key), "e%u", i);
    alarms[i].enabled = prefs.getBool(key, alarms[i].enabled);
  }
}

void drawText(int x, int y, const char *s, uint16_t fg, uint16_t bg, uint8_t size = 2)
{
  M5.Lcd.setTextSize(size);
  M5.Lcd.setTextColor(fg, bg);
  M5.Lcd.setCursor(x, y);
  M5.Lcd.print(s);
}

void applyScreenRotation()
{
  M5.Lcd.setRotation(rotate180 ? 3 : 1);
}

void toggleScreenRotation()
{
  rotate180 = !rotate180;
  applyScreenRotation();
  saveSettings();
  markDirty();
}

bool getNow(tm *out)
{
  return getLocalTime(out, 20);
}

bool syncTime()
{
  lastNtpAttemptMs = millis();
  if (WiFi.status() != WL_CONNECTED) {
    timeSynced = false;
    return false;
  }

  configTime(kChinaGmtOffsetSeconds, 0, "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");

  tm now{};
  timeSynced = getLocalTime(&now, 5000);
  return timeSynced;
}

void drawBootStatus(const char *line1, const char *line2)
{
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(18, 52);
  M5.Lcd.print(line1);
  M5.Lcd.setCursor(18, 88);
  M5.Lcd.print(line2);
}

void setupWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  drawBootStatus("Connecting WiFi...", "Skip after 6 sec");

  WiFi.begin();
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 6000) {
    M5.update();
    if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed()) {
      break;
    }
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    syncTime();
  } else {
    WiFi.disconnect(false);
    timeSynced = false;
    drawBootStatus("WiFi offline", "Use WiFi ADD later");
    delay(800);
  }
}

void startWifiPortal()
{
  M5.Lcd.fillScreen(TFT_BLACK);
  drawText(18, 42, "WiFi setup portal", TFT_WHITE, TFT_BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.setCursor(18, 78);
  M5.Lcd.print("AP: M5Core-Alarm");
  M5.Lcd.setCursor(18, 110);
  M5.Lcd.print("Open 192.168.4.1");

  wifiManager.setConfigPortalTimeout(240);
  wifiManager.startConfigPortal("M5Core-Alarm");
  syncTime();
  markDirty();
}

void scanWifiNetworks()
{
  M5.Lcd.fillScreen(TFT_BLACK);
  drawText(18, 42, "Scanning WiFi...", TFT_WHITE, TFT_BLACK);
  WiFi.mode(WIFI_STA);
  WiFi.scanDelete();
  const int found = WiFi.scanNetworks(false, true);
  wifiScanCount = 0;
  if (found > 0) {
    const uint8_t limit = min(found, static_cast<int>(kWifiMaxScanRows));
    for (uint8_t i = 0; i < limit; ++i) {
      wifiSsids[i] = WiFi.SSID(i);
      wifiRssi[i] = WiFi.RSSI(i);
      wifiScanCount++;
    }
  }
  selectedWifi = 0;
  page = Page::WiFiScan;
  markDirty();
}

void drawFooter(const char *a, const char *b, const char *c)
{
  M5.Lcd.fillRect(0, 205, 320, 35, TFT_DARKGREY);
  drawText(18, 216, a, TFT_WHITE, TFT_DARKGREY);
  drawText(132, 216, b, TFT_WHITE, TFT_DARKGREY);
  drawText(246, 216, c, TFT_WHITE, TFT_DARKGREY);
}

void drawHeader(const char *title)
{
  M5.Lcd.fillRect(0, 0, 320, 30, TFT_NAVY);
  drawText(8, 7, title, TFT_WHITE, TFT_NAVY);

  M5.Lcd.setCursor(242, 7);
  if (WiFi.status() == WL_CONNECTED) {
    M5.Lcd.print(timeSynced ? "NTP" : "WiFi");
  } else {
    M5.Lcd.print("OFF");
  }
}

const char *mainModeName(uint8_t mode)
{
  switch (mode) {
    case 0: return "TIME";
    case 1: return "ALARM";
    case 2: return "STOPWATCH";
    case 3: return "COUNTDOWN";
    case 4: return "POMODORO";
    case 5: return "WiFi";
    case 6: return "ROTATE";
  }
  return "";
}

void formatDuration(uint32_t totalSeconds, char *out, size_t outSize)
{
  const uint32_t minutes = totalSeconds / 60;
  const uint32_t seconds = totalSeconds % 60;
  snprintf(out, outSize, "%02lu:%02lu", static_cast<unsigned long>(minutes), static_cast<unsigned long>(seconds));
}

const char *weekdayName(const tm &now)
{
  const uint8_t day = now.tm_wday % 7;
  const char *names[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  return names[day];
}

uint16_t weekdayColor(const tm &now)
{
  if (now.tm_wday == 0) {
    return TFT_RED;
  }
  if (now.tm_wday == 6) {
    return TFT_GREEN;
  }
  return TFT_LIGHTGREY;
}

int nextAlarmIndex(const tm &now)
{
  int best = -1;
  int bestDelta = 24 * 60 + 1;
  const int minuteNow = now.tm_hour * 60 + now.tm_min;

  for (uint8_t i = 0; i < kAlarmCount; ++i) {
    if (!alarms[i].enabled) {
      continue;
    }
    const int alarmMinute = alarms[i].hour * 60 + alarms[i].minute;
    int delta = alarmMinute - minuteNow;
    if (delta < 0) {
      delta += 24 * 60;
    }
    if (delta < bestDelta) {
      bestDelta = delta;
      best = i;
    }
  }
  return best;
}

void drawNextAlarmLine(const tm &now, int y)
{
  const int next = nextAlarmIndex(now);
  M5.Lcd.setTextSize(2);
  if (next >= 0) {
    M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
    char alarmText[28];
    snprintf(alarmText, sizeof(alarmText), "Next %02u:%02u", alarms[next].hour, alarms[next].minute);
    M5.Lcd.setCursor(98, y);
    M5.Lcd.print(alarmText);
  } else {
    M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Lcd.setCursor(94, y);
    M5.Lcd.print("No alarm on");
  }
}

void drawDateLine(const tm &now, int x, int y, uint16_t color = TFT_LIGHTGREY)
{
  char dateText[32];
  snprintf(dateText, sizeof(dateText), "%04d-%02d-%02d  UTC+8", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(color, TFT_BLACK);
  M5.Lcd.setCursor(x, y);
  M5.Lcd.print(dateText);
}

void drawDigitalHome(const tm &now)
{
  if (currentFullRedraw) {
    M5.Lcd.fillRect(0, 48, 320, 138, TFT_BLACK);
  }
  char timeText[9];
  if (selectedMainMode == 6) {
    snprintf(timeText, sizeof(timeText), "ROT %03d", rotate180 ? 180 : 0);
  } else {
    snprintf(timeText, sizeof(timeText), "%02d:%02d:%02d", now.tm_hour, now.tm_min, now.tm_sec);
  }
  M5.Lcd.setTextColor(ringing ? TFT_YELLOW : TFT_WHITE, TFT_BLACK);
  M5.Lcd.setTextSize(6);
  M5.Lcd.setCursor(18, 54);
  M5.Lcd.print(timeText);

  if (currentFullRedraw) {
    drawDateLine(now, 28, 124);
    drawNextAlarmLine(now, 154);
  }
}

void drawRetroHome(const tm &now)
{
  if (currentFullRedraw) {
    M5.Lcd.fillRect(0, 30, 320, 175, TFT_BLACK);
    M5.Lcd.drawRect(12, 42, 296, 118, TFT_DARKGREY);
    M5.Lcd.drawRect(18, 48, 284, 106, TFT_DARKGREEN);
    M5.Lcd.drawRoundRect(16, 48, 288, 92, 8, TFT_DARKGREEN);
    M5.Lcd.fillRoundRect(22, 54, 276, 80, 6, TFT_BLACK);
  }

  char timeText[6];
  snprintf(timeText, sizeof(timeText), "%02d:%02d", now.tm_hour, now.tm_min);
  M5.Lcd.setTextSize(7);
  M5.Lcd.setTextColor(ringing ? TFT_YELLOW : TFT_GREEN, TFT_BLACK);
  M5.Lcd.setCursor(42, 70);
  M5.Lcd.print(timeText);

  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(TFT_DARKGREEN, TFT_BLACK);
  M5.Lcd.setCursor(248, 112);
  M5.Lcd.printf("%02d", now.tm_sec);

  if (currentFullRedraw) {
    drawDateLine(now, 34, 164, TFT_GREEN);
    drawNextAlarmLine(now, 186);
  }
}

void drawDialHand(float angle, int length, uint16_t color, int width)
{
  const int cx = 160;
  const int cy = 104;
  const int x = cx + static_cast<int>(cos(angle) * length);
  const int y = cy + static_cast<int>(sin(angle) * length);
  for (int i = -width; i <= width; ++i) {
    M5.Lcd.drawLine(cx + i, cy, x + i, y, color);
  }
}

void drawDialHome(const tm &now)
{
  if (!currentFullRedraw) {
    char timeText[6];
    snprintf(timeText, sizeof(timeText), "%02d:%02d", now.tm_hour, now.tm_min);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.setCursor(132, 176);
    M5.Lcd.print(timeText);
    return;
  }
  M5.Lcd.fillRect(0, 32, 320, 170, TFT_BLACK);
  const int cx = 160;
  const int cy = 104;
  const int r = 68;
  M5.Lcd.fillCircle(cx, cy, r + 8, TFT_DARKGREY);
  M5.Lcd.fillCircle(cx, cy, r + 4, TFT_BLACK);
  M5.Lcd.drawCircle(cx, cy, r, TFT_WHITE);
  M5.Lcd.drawCircle(cx, cy, r - 1, TFT_LIGHTGREY);

  for (int i = 0; i < 12; ++i) {
    const float angle = (i / 12.0f) * 2.0f * PI - PI / 2.0f;
    const int x1 = cx + static_cast<int>(cos(angle) * (r - 8));
    const int y1 = cy + static_cast<int>(sin(angle) * (r - 8));
    const int x2 = cx + static_cast<int>(cos(angle) * (r - 2));
    const int y2 = cy + static_cast<int>(sin(angle) * (r - 2));
    M5.Lcd.drawLine(x1, y1, x2, y2, i % 3 == 0 ? TFT_WHITE : TFT_DARKGREY);
  }

  const float hourAngle = ((now.tm_hour % 12) + now.tm_min / 60.0f) / 12.0f * 2.0f * PI - PI / 2.0f;
  const float minuteAngle = (now.tm_min / 60.0f) * 2.0f * PI - PI / 2.0f;
  const float secondAngle = (now.tm_sec / 60.0f) * 2.0f * PI - PI / 2.0f;
  drawDialHand(hourAngle, 36, TFT_WHITE, 2);
  drawDialHand(minuteAngle, 54, TFT_LIGHTGREY, 1);
  drawDialHand(secondAngle, 58, ringing ? TFT_YELLOW : TFT_RED, 0);
  M5.Lcd.fillCircle(cx, cy, 4, TFT_WHITE);

  char timeText[6];
  snprintf(timeText, sizeof(timeText), "%02d:%02d", now.tm_hour, now.tm_min);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.setCursor(132, 176);
  M5.Lcd.print(timeText);

  drawDateLine(now, 44, 150);
  drawNextAlarmLine(now, 194);
}

void drawHome()
{
  tm now{};
  getNow(&now);
  if (currentFullRedraw) {
    char modeTitle[24];
    snprintf(modeTitle, sizeof(modeTitle), "MODE %u/%u %s", selectedMainMode + 1, kMainModeCount, mainModeName(selectedMainMode));
    drawHeader(modeTitle);
    M5.Lcd.fillRect(0, 30, 320, 175, TFT_BLACK);
    M5.Lcd.drawRoundRect(18, 48, 284, 94, 8, TFT_LIGHTGREY);
    M5.Lcd.drawRoundRect(28, 62, 264, 62, 4, TFT_DARKGREY);
  }

  char timeText[9];
  snprintf(timeText, sizeof(timeText), "%02d:%02d:%02d", now.tm_hour, now.tm_min, now.tm_sec);
  M5.Lcd.setTextSize(6);
  M5.Lcd.setTextColor(ringing ? TFT_YELLOW : TFT_GREEN, TFT_BLACK);
  M5.Lcd.setCursor(20, 70);
  M5.Lcd.print(timeText);

  if (currentFullRedraw) {
    char dateText[28];
    snprintf(dateText, sizeof(dateText), "%04d/%02d/%02d %s", now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, weekdayName(now));
    M5.Lcd.fillRect(20, 144, 280, 32, TFT_BLACK);
    drawText(34, 154, dateText, weekdayColor(now), TFT_BLACK, 2);
    M5.Lcd.fillRect(60, 178, 200, 20, TFT_BLACK);
    if (!ringing) {
      drawNextAlarmLine(now, 180);
    }
  }

  if (ringing) {
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Lcd.fillRect(74, 172, 172, 26, TFT_BLACK);
    M5.Lcd.setCursor(112, 180);
    M5.Lcd.print("WAKE UP");
  }

  if (currentFullRedraw) {
    drawFooter("MODE", "OK", "SET");
  }
}

void drawAlarmList()
{
  drawHeader("Alarm List");

  for (uint8_t i = 0; i < kAlarmMenuCount; ++i) {
    const int y = 36 + i * 27;
    const bool selected = i == selectedAlarm;
    M5.Lcd.fillRect(10, y - 4, 300, 28, selected ? TFT_DARKCYAN : TFT_BLACK);
    M5.Lcd.setTextSize(2);
    if (i == kAlarmCount) {
      M5.Lcd.setTextColor(TFT_LIGHTGREY, selected ? TFT_DARKCYAN : TFT_BLACK);
      drawText(20, y, "Back", TFT_LIGHTGREY,
                    selected ? TFT_DARKCYAN : TFT_BLACK);
      continue;
    }
    M5.Lcd.setTextColor(alarms[i].enabled ? TFT_WHITE : TFT_DARKGREY,
                        selected ? TFT_DARKCYAN : TFT_BLACK);
    char row[42];
    snprintf(row, sizeof(row), "%u  %02u:%02u  %-7s  %s",
             i + 1,
             alarms[i].hour,
             alarms[i].minute,
             kBuiltinMusicName,
             alarms[i].enabled ? "ON" : "OFF");
    M5.Lcd.setCursor(20, y);
    M5.Lcd.print(row);
  }

  drawFooter("PREV", "OK", "NEXT");
}

const char *editFieldName()
{
  switch (editField) {
    case EditField::Hour: return "Hour";
    case EditField::Minute: return "Minute";
    case EditField::Preview: return "Preview";
    case EditField::Enabled: return "Enabled";
    case EditField::Done: return "Done";
  }
  return "";
}

void drawAlarmEdit()
{
  drawHeader("Edit Alarm");

  const Alarm &alarm = alarms[selectedAlarm];
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5.Lcd.setCursor(24, 44);
  M5.Lcd.printf("Alarm %u", selectedAlarm + 1);

  M5.Lcd.setTextSize(5);
  M5.Lcd.setTextColor(alarm.enabled ? TFT_WHITE : TFT_DARKGREY, TFT_BLACK);
  M5.Lcd.setCursor(70, 76);
  M5.Lcd.printf("%02u:%02u", alarm.hour, alarm.minute);

  if (editField == EditField::Hour) {
    M5.Lcd.drawFastHLine(70, 124, 70, TFT_ORANGE);
  } else if (editField == EditField::Minute) {
    M5.Lcd.drawFastHLine(178, 124, 70, TFT_ORANGE);
  }

  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(editField == EditField::Preview ? TFT_ORANGE : TFT_CYAN, TFT_BLACK);
  M5.Lcd.setCursor(44, 146);
  M5.Lcd.printf("Preview: %s", kBuiltinMusicName);

  M5.Lcd.setTextColor(editField == EditField::Enabled ? TFT_ORANGE : TFT_CYAN, TFT_BLACK);
  M5.Lcd.setCursor(44, 172);
  M5.Lcd.printf("Enabled: %s", alarm.enabled ? "YES" : "NO");

  M5.Lcd.setTextColor(editField == EditField::Done ? TFT_ORANGE : TFT_LIGHTGREY, TFT_BLACK);
  M5.Lcd.setCursor(226, 172);
  M5.Lcd.print("Done");

  M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5.Lcd.setCursor(112, 44);
  M5.Lcd.printf("[%s]", editFieldName());

  drawFooter("FIELD", "OK/+", "-");
}

void drawSoundPreview()
{
  drawHeader("Ring Preview");

  Alarm &alarm = alarms[selectedAlarm];
  M5.Lcd.setTextSize(3);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.setCursor(50, 64);
  M5.Lcd.printf("Alarm %u", selectedAlarm + 1);

  M5.Lcd.setTextSize(4);
  M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
  M5.Lcd.setCursor(64, 112);
  M5.Lcd.print(kBuiltinMusicName);

  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5.Lcd.setCursor(42, 170);
  M5.Lcd.print("Music only, C replay");

  drawFooter("BACK", "OK", "REPLAY");
}

void drawStopwatch()
{
  drawHeader("Stopwatch");
  M5.Lcd.fillRect(0, 64, 320, 116, TFT_BLACK);

  uint32_t elapsed = stopwatchElapsedMs;
  if (stopwatchRunning) {
    elapsed += millis() - stopwatchStartedMs;
  }

  const uint32_t totalCentis = elapsed / 10;
  const uint32_t minutes = totalCentis / 6000;
  const uint32_t seconds = (totalCentis / 100) % 60;
  const uint32_t centis = totalCentis % 100;

  M5.Lcd.setTextSize(6);
  M5.Lcd.setTextColor(stopwatchRunning ? TFT_GREEN : TFT_WHITE, TFT_BLACK);
  M5.Lcd.setCursor(26, 78);
  M5.Lcd.printf("%02lu:%02lu", static_cast<unsigned long>(minutes), static_cast<unsigned long>(seconds));

  M5.Lcd.setTextSize(3);
  M5.Lcd.setCursor(236, 108);
  M5.Lcd.printf(".%02lu", static_cast<unsigned long>(centis));

  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5.Lcd.setCursor(112, 158);
  M5.Lcd.print(stopwatchRunning ? "running" : "paused");

  drawFooter("BACK", stopwatchRunning ? "PAUSE" : "START", "RESET");
}

uint32_t currentCountdownRemaining()
{
  if (!countdownRunning) {
    return countdownRemainingSec;
  }
  const uint32_t elapsed = (millis() - countdownStartedMs) / 1000;
  return elapsed >= countdownRemainingSec ? 0 : countdownRemainingSec - elapsed;
}

void drawCountdown()
{
  drawHeader("Countdown");
  M5.Lcd.fillRect(0, 64, 320, 126, TFT_BLACK);

  char text[8];
  formatDuration(currentCountdownRemaining(), text, sizeof(text));
  M5.Lcd.setTextSize(7);
  M5.Lcd.setTextColor(countdownRunning ? TFT_YELLOW : TFT_WHITE, TFT_BLACK);
  M5.Lcd.setCursor(42, 76);
  M5.Lcd.print(text);

  char durationText[24];
  formatDuration(countdownDurationSec, durationText, sizeof(durationText));
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5.Lcd.setCursor(78, 162);
  M5.Lcd.printf("Preset %s", durationText);

  drawFooter("BACK", countdownRunning ? "PAUSE" : "START", countdownRunning ? "RESET" : "+1M");
}

void drawPomodoro()
{
  drawHeader("Pomodoro");
  M5.Lcd.fillRect(0, 60, 320, 130, TFT_BLACK);

  char text[8];
  uint32_t remaining = pomodoroRemainingSec;
  if (pomodoroRunning) {
    const uint32_t elapsed = (millis() - pomodoroStartedMs) / 1000;
    remaining = elapsed >= pomodoroRemainingSec ? 0 : pomodoroRemainingSec - elapsed;
  }
  formatDuration(remaining, text, sizeof(text));
  M5.Lcd.setTextSize(7);
  M5.Lcd.setTextColor(pomodoroWorkPhase ? TFT_RED : TFT_GREEN, TFT_BLACK);
  M5.Lcd.setCursor(42, 72);
  M5.Lcd.print(text);

  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5.Lcd.setCursor(82, 156);
  M5.Lcd.printf("%s  round %u", pomodoroWorkPhase ? "Focus" : "Break", pomodoroRound);

  drawFooter("BACK", pomodoroRunning ? "PAUSE" : "START", "SKIP");
}

void drawWiFiStatus()
{
  drawHeader("WiFi / Time");

  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.setCursor(24, 48);
  M5.Lcd.printf("WiFi: %s", WiFi.status() == WL_CONNECTED ? "connected" : "offline");

  M5.Lcd.setCursor(24, 78);
  M5.Lcd.printf("SSID: %s", WiFi.SSID().length() ? WiFi.SSID().c_str() : "-");

  M5.Lcd.setCursor(24, 108);
  M5.Lcd.printf("IP: %s", WiFi.localIP().toString().c_str());

  M5.Lcd.setCursor(24, 138);
  M5.Lcd.printf("Time: %s", timeSynced ? "NTP synced" : "not synced");

  M5.Lcd.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5.Lcd.setCursor(24, 170);
  M5.Lcd.print("B scan, C add WiFi");

  drawFooter("BACK", "SCAN", "ADD");
}

void drawWiFiScan()
{
  drawHeader("WiFi Scan");

  const uint8_t totalRows = wifiScanCount + 2;
  for (uint8_t i = 0; i < totalRows; ++i) {
    const int y = 38 + i * 26;
    const bool selected = i == selectedWifi;
    M5.Lcd.fillRect(10, y - 4, 300, 25, selected ? TFT_DARKCYAN : TFT_BLACK);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(selected ? TFT_WHITE : TFT_LIGHTGREY, selected ? TFT_DARKCYAN : TFT_BLACK);

    if (i < wifiScanCount) {
      const String shortSsid = wifiSsids[i].substring(0, 20);
      char row[36];
      snprintf(row, sizeof(row), "%-20s %ld", shortSsid.c_str(), static_cast<long>(wifiRssi[i]));
      M5.Lcd.setCursor(18, y);
      M5.Lcd.print(row);
    } else if (i == wifiScanCount) {
      drawText(18, y, "Add WiFi", selected ? TFT_WHITE : TFT_LIGHTGREY,
                    selected ? TFT_DARKCYAN : TFT_BLACK);
    } else {
      drawText(18, y, "Back", selected ? TFT_WHITE : TFT_LIGHTGREY,
                    selected ? TFT_DARKCYAN : TFT_BLACK);
    }
  }

  if (wifiScanCount == 0) {
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(TFT_DARKGREY, TFT_BLACK);
    M5.Lcd.setCursor(72, 86);
    M5.Lcd.print("No network");
  }

  drawFooter("PREV", "OK", "NEXT");
}

bool pageNeedsPeriodicRefresh()
{
  return page == Page::Home ||
         page == Page::Stopwatch ||
         page == Page::Countdown ||
         page == Page::Pomodoro;
}

void drawPage(bool clearScreen)
{
  currentFullRedraw = clearScreen;
  if (clearScreen) {
    M5.Lcd.fillScreen(TFT_BLACK);
  }
  switch (page) {
    case Page::Home: drawHome(); break;
    case Page::AlarmList: drawAlarmList(); break;
    case Page::AlarmEdit: drawAlarmEdit(); break;
    case Page::SoundPreview: drawSoundPreview(); break;
    case Page::Stopwatch: drawStopwatch(); break;
    case Page::Countdown: drawCountdown(); break;
    case Page::Pomodoro: drawPomodoro(); break;
    case Page::WiFiStatus: drawWiFiStatus(); break;
    case Page::WiFiScan: drawWiFiScan(); break;
  }
  needsFullDraw = false;
  currentFullRedraw = true;
}

void stopAlarm()
{
  ringing = false;
  ringingAlarm = -1;
  stopBuiltinMusic();
  M5.Speaker.mute();
  ledsOff();
  markDirty();
}

void startAlarm(uint8_t index)
{
  stopBuiltinMusic();
  ringing = true;
  ringingAlarm = index;
  ringingStartedMs = millis();
  markDirty();
}

void startToolAlert()
{
  startAlarm(255);
  ringingAlarm = -1;
}

void playSoundPattern()
{
  if (!ringing) {
    return;
  }

  if (millis() - ringingStartedMs > kAlarmTimeoutMs) {
    stopAlarm();
    return;
  }

  startBuiltinMusic();
}

void checkAlarms()
{
  if (ringing) {
    playSoundPattern();
    return;
  }

  tm now{};
  if (!getNow(&now)) {
    return;
  }

  const int minuteOfDay = now.tm_hour * 60 + now.tm_min;
  for (uint8_t i = 0; i < kAlarmCount; ++i) {
    if (!alarms[i].enabled) {
      continue;
    }
    const int alarmMinuteOfDay = alarms[i].hour * 60 + alarms[i].minute;
    if (alarmMinuteOfDay == minuteOfDay &&
        now.tm_sec == 0 &&
        (lastAlarmDay != now.tm_mday || lastAlarmMinuteOfDay != minuteOfDay)) {
      lastAlarmDay = now.tm_mday;
      lastAlarmMinuteOfDay = minuteOfDay;
      startAlarm(i);
      break;
    }
  }
}

void cyclePageFromHome()
{
  page = Page::AlarmList;
  markDirty();
}

void openSelectedHomeAction()
{
  switch (selectedMainMode) {
    case 0:
      syncTime();
      break;
    case 1:
      selectedAlarm = 0;
      page = Page::AlarmList;
      break;
    case 2:
      page = Page::Stopwatch;
      break;
    case 3:
      page = Page::Countdown;
      break;
    case 4:
      page = Page::Pomodoro;
      break;
    case 5:
      page = Page::WiFiStatus;
      break;
    case 6:
      toggleScreenRotation();
      return;
  }
  markDirty();
}

void handleHomeButtons()
{
  if (M5.BtnA.wasPressed()) {
    selectedMainMode = (selectedMainMode + 1) % kMainModeCount;
    markDirty();
  } else if (M5.BtnB.wasPressed()) {
    openSelectedHomeAction();
  } else if (M5.BtnC.wasPressed()) {
    openSelectedHomeAction();
  }
}

void handleAlarmListButtons()
{
  if (M5.BtnA.wasPressed()) {
    selectedAlarm = (selectedAlarm + kAlarmMenuCount - 1) % kAlarmMenuCount;
    markDirty();
  } else if (M5.BtnB.wasPressed()) {
    if (selectedAlarm == kAlarmCount) {
      page = Page::Home;
      markDirty();
      return;
    }
    page = Page::AlarmEdit;
    editField = EditField::Hour;
    markDirty();
  } else if (M5.BtnC.wasPressed()) {
    selectedAlarm = (selectedAlarm + 1) % kAlarmMenuCount;
    markDirty();
  }
}

void changeSelectedValue(int delta)
{
  Alarm &alarm = alarms[selectedAlarm];

  switch (editField) {
    case EditField::Hour:
      alarm.hour = (alarm.hour + delta + 24) % 24;
      break;
    case EditField::Minute:
      alarm.minute = (alarm.minute + delta + 60) % 60;
      break;
    case EditField::Preview:
      break;
    case EditField::Enabled:
      alarm.enabled = !alarm.enabled;
      break;
    case EditField::Done:
      break;
  }

  saveSettings();
  markDirty();
}

void handleAlarmEditButtons()
{
  if (M5.BtnA.wasPressed()) {
    editField = static_cast<EditField>((static_cast<int>(editField) + 1) % 5);
    markDirty();
  } else if (M5.BtnB.wasPressed()) {
    if (editField == EditField::Done) {
      page = Page::AlarmList;
      markDirty();
    } else if (editField == EditField::Preview) {
      page = Page::SoundPreview;
      markDirty();
    } else if (editField == EditField::Enabled) {
      alarms[selectedAlarm].enabled = !alarms[selectedAlarm].enabled;
      saveSettings();
      markDirty();
    } else {
      changeSelectedValue(1);
    }
  } else if (M5.BtnC.wasPressed()) {
    if (editField == EditField::Preview) {
      editField = EditField::Enabled;
      markDirty();
    } else {
      changeSelectedValue(-1);
    }
  }
}

void handleSoundPreviewButtons()
{
  if (M5.BtnA.wasPressed()) {
    stopBuiltinMusic();
    M5.Speaker.mute();
    page = Page::AlarmEdit;
    editField = EditField::Enabled;
    markDirty();
    return;
  } else if (M5.BtnB.wasPressed()) {
    stopBuiltinMusic();
    M5.Speaker.mute();
    page = Page::AlarmEdit;
    editField = EditField::Enabled;
    markDirty();
    return;
  } else if (M5.BtnC.wasPressed()) {
    if (builtinMusicPlaying) {
      builtinMusicRestartRequested = true;
    } else {
      startBuiltinMusic();
    }
    markDirty();
    return;
  }

  if (!ringing) {
    serviceLedFlash(true);
    startBuiltinMusic();
  }
}

void handleStopwatchButtons()
{
  if (M5.BtnA.wasPressed()) {
    page = Page::Home;
    markDirty();
  } else if (M5.BtnB.wasPressed()) {
    if (stopwatchRunning) {
      stopwatchElapsedMs += millis() - stopwatchStartedMs;
      stopwatchRunning = false;
    } else {
      stopwatchStartedMs = millis();
      stopwatchRunning = true;
    }
    markDirty();
  } else if (M5.BtnC.wasPressed()) {
    stopwatchRunning = false;
    stopwatchElapsedMs = 0;
    markDirty();
  }
}

void handleCountdownButtons()
{
  if (M5.BtnA.wasPressed()) {
    page = Page::Home;
    markDirty();
  } else if (M5.BtnB.wasPressed()) {
    if (countdownRunning) {
      countdownRemainingSec = currentCountdownRemaining();
      countdownRunning = false;
    } else if (countdownRemainingSec > 0) {
      countdownStartedMs = millis();
      countdownRunning = true;
    }
    markDirty();
  } else if (M5.BtnC.wasPressed()) {
    if (countdownRunning) {
      countdownRunning = false;
      countdownRemainingSec = countdownDurationSec;
    } else {
      countdownDurationSec += 60;
      if (countdownDurationSec > 99 * 60) {
        countdownDurationSec = 60;
      }
      countdownRemainingSec = countdownDurationSec;
      saveSettings();
    }
    markDirty();
  }
}

void handlePomodoroButtons()
{
  if (M5.BtnA.wasPressed()) {
    page = Page::Home;
    markDirty();
  } else if (M5.BtnB.wasPressed()) {
    if (pomodoroRunning) {
      const uint32_t elapsed = (millis() - pomodoroStartedMs) / 1000;
      pomodoroRemainingSec = elapsed >= pomodoroRemainingSec ? 0 : pomodoroRemainingSec - elapsed;
      pomodoroRunning = false;
    } else {
      pomodoroStartedMs = millis();
      pomodoroRunning = true;
    }
    markDirty();
  } else if (M5.BtnC.wasPressed()) {
    pomodoroWorkPhase = !pomodoroWorkPhase;
    if (pomodoroWorkPhase) {
      pomodoroRound++;
    }
    pomodoroRemainingSec = pomodoroWorkPhase ? 25 * 60 : 5 * 60;
    pomodoroStartedMs = millis();
    markDirty();
  }
}

void handleWiFiButtons()
{
  if (M5.BtnA.wasPressed()) {
    page = Page::Home;
    markDirty();
  } else if (M5.BtnB.wasPressed()) {
    scanWifiNetworks();
  } else if (M5.BtnC.wasPressed()) {
    wifiManager.resetSettings();
    startWifiPortal();
  }
}

void handleWiFiScanButtons()
{
  const uint8_t totalRows = wifiScanCount + 2;
  if (M5.BtnA.wasPressed()) {
    selectedWifi = (selectedWifi + totalRows - 1) % totalRows;
    markDirty();
  } else if (M5.BtnB.wasPressed()) {
    if (selectedWifi == wifiScanCount + 1) {
      page = Page::WiFiStatus;
      markDirty();
      return;
    }
    startWifiPortal();
  } else if (M5.BtnC.wasPressed()) {
    selectedWifi = (selectedWifi + 1) % totalRows;
    markDirty();
  }
}

void handleButtons()
{
  if (ringing &&
      (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed() ||
       M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed())) {
    stopAlarm();
    return;
  }

  switch (page) {
    case Page::Home: handleHomeButtons(); break;
    case Page::AlarmList: handleAlarmListButtons(); break;
    case Page::AlarmEdit: handleAlarmEditButtons(); break;
    case Page::SoundPreview: handleSoundPreviewButtons(); break;
    case Page::Stopwatch: handleStopwatchButtons(); break;
    case Page::Countdown: handleCountdownButtons(); break;
    case Page::Pomodoro: handlePomodoroButtons(); break;
    case Page::WiFiStatus: handleWiFiButtons(); break;
    case Page::WiFiScan: handleWiFiScanButtons(); break;
  }
}

void serviceCountdown()
{
  if (!countdownRunning) {
    return;
  }

  const uint32_t remaining = currentCountdownRemaining();
  if (remaining == 0) {
    countdownRunning = false;
    countdownRemainingSec = countdownDurationSec;
    startToolAlert();
  }
}

void servicePomodoro()
{
  if (!pomodoroRunning) {
    return;
  }

  const uint32_t elapsed = (millis() - pomodoroStartedMs) / 1000;
  if (elapsed < pomodoroRemainingSec) {
    return;
  }

  pomodoroRunning = false;
  pomodoroWorkPhase = !pomodoroWorkPhase;
  if (pomodoroWorkPhase) {
    pomodoroRound++;
  }
  pomodoroRemainingSec = pomodoroWorkPhase ? 25 * 60 : 5 * 60;
  pomodoroStartedMs = millis();
  startToolAlert();
}

}  // namespace

void setup()
{
  M5.begin(true, false, true, true);
  M5.Power.begin();
  M5.Speaker.setVolume(4);
  Serial.begin(115200);
  initLeds();

  prefs.begin("m5alarm", false);
  loadSettings();
  applyScreenRotation();

  setupWiFi();
  drawPage(true);
}

void loop()
{
  M5.update();
  handleButtons();
  checkAlarms();
  serviceLedFlash(ringing || page == Page::SoundPreview);
  serviceCountdown();
  servicePomodoro();

  if (WiFi.status() == WL_CONNECTED && !timeSynced && millis() - lastNtpAttemptMs > kNtpRetryMs) {
    syncTime();
    markDirty();
  }

  if (needsFullDraw) {
    lastDrawMs = millis();
    drawPage(true);
  } else if (pageNeedsPeriodicRefresh()) {
    const uint32_t refreshMs = (page == Page::Home || ringing) ? kHomeRefreshMs : kToolRefreshMs;
    if (millis() - lastDrawMs < refreshMs) {
      delay(10);
      return;
    }
    lastDrawMs = millis();
    drawPage(false);
  }

  delay(10);
}
