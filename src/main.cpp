#include <Arduino.h>
#include <LiquidCrystal.h>
#include <Wire.h>
#include <GyverTimer.h>
#include <SettingsESP.h>
#include <WiFiConnector.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <GyverDS18.h>
#include <Adafruit_AHTX0.h>
#include <GyverFilters.h>
#include <GyverButton.h>
#include <GyverMenu.h>
#include "time.h"

#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <esp_wifi.h>

// Пины LCD1602 P1620B
#define PIN_RS 19
#define PIN_E  23
#define PIN_D4 18
#define PIN_D5 5
#define PIN_D6 4
#define PIN_D7 2

#define PIN_KEYPAD 35   // GPIO 35 (Аналоговая клавиатура)
#define PIN_DS18B20 16  // GPIO 16 (Временный эталонный DS18B20)
#define AHT10_I2C_ADDR 0x38

LiquidCrystal lcd(PIN_RS, PIN_E, PIN_D4, PIN_D5, PIN_D6, PIN_D7);
Preferences prefs;
GyverDS18Single ds18(PIN_DS18B20);
Adafruit_AHTX0 aht;
GyverMenu menu(16, 2);
void buildLcdMenu(gm::Builder& b);
void printCyrillicChar(uint8_t c);
void startDsCalib();
void startAhtCalib();

// --- ФИЛЬТРЫ GYVERFILTERS ДЛЯ СГЛАЖИВАНИЯ ДАТЧИКОВ (Running Average + Median) ---
GFilterRA tempFilter(0.1f);
GFilterRA ahtTempFilter(0.1f);
GFilterRA ahtHumFilter(0.1f);
GMedian3<float> ahtHumMedian;
GMedian3<float> ahtTempMedian;

// --- ДАННЫЕ НАСТРОЕК ---
struct ConfigData {
  float tempOffset = 0.0f;
  float humOffset = 0.0f;
  char wifiSsid[32] = "Salomanov";
  char wifiPass[32] = "salomanov";
  int gmtOffsetHours = 5; // GMT+5

  // Настройки и калибровки аналоговой клавиатуры
  int keypadPin = 35;
  int adcCenter = 248;
  int adcMenu   = 372;
  int adcLeft   = 682;
  int adcRight  = 1191;
  int adcBack   = 1985;
  int adcTol    = 70; // Допуск ± ADC

  // Источник температуры для экрана (0: AHT10, 1: DS18B20, 2: Оба (AHT+DS18), 3: Среднее)
  uint8_t dispTempSource = 0;
};

ConfigData cfg;
SettingsESP sett("Инкубатор & Сушка");

bool ahtFound = false;
int settingSubItem = 0; // 0..5 пункты меню настроек
bool settingEditMode = false; // true - режим правки числа

float currentTemp = NAN;
float currentHum = NAN;
float lastRawTemp = NAN;
float lastRawHum = NAN;
bool climateOk = false;

void updateDisplayWindow();

// --- ВРЕМЕННЫЙ ЭТАЛОН DS18B20 И АВТОКАЛИБРОВКА ---
bool ds18Ok = false;
float tempDS18 = NAN;
float calDsOffset = 0.0f; // Поправка самого DS18B20

#define DS_STABLE_COUNT 60
#define DS_STABLE_DELTA 0.15f
#define DS_CALIB_TIMEOUT 300000UL
#define DS_BODY_TARGET 36.6f

int dsCalibState = 0; // 0-idle, 1-working, 2-success, 3-error
uint32_t dsCalibStartMs = 0;
uint8_t dsCalibStableCount = 0;
float dsCalibPrev[DS_STABLE_COUNT];
uint8_t dsCalibPrevIdx = 0;
float dsCalibResult = NAN;
String dsCalibStatus = "Не запускалась";

int ahtCalibState = 0; // 0-idle, 1-working, 2-success, 3-error
uint32_t ahtCalibStartMs = 0;
uint8_t ahtCalibStableCount = 0;
float ahtCalibPrevDS[DS_STABLE_COUNT];
float ahtCalibPrevAHT[DS_STABLE_COUNT];
uint8_t ahtCalibPrevIdx = 0;
float ahtCalibResult = NAN;
String ahtCalibStatus = "Не запускалась";

// --- ЭКСПРЕСС-ТЕСТ ВЛАЖНОСТИ AHT10 (ДЫХАНИЕМ) ---
int humTestState = 0; // 0-idle, 1-working, 2-success, 3-error
uint32_t humTestStartMs = 0;
float humTestStartVal = 0.0f;
String humTestStatus = "Не запускался";

void startHumTest() {
  if (!ahtFound || isnan(currentHum)) {
    humTestState = 3;
    humTestStatus = "Ошибка: датчик AHT10 отключен";
    return;
  }
  humTestState = 1;
  humTestStartMs = millis();
  humTestStartVal = currentHum;
  humTestStatus = "Подуйте на датчик (ожидание изменений ±5%)";
  Serial.println("[AHT10] Тест влажности запущен: ожидание изменения");
}

void tickHumTest(float rawHum) {
  if (humTestState != 1) return;

  uint32_t elapsed = millis() - humTestStartMs;
  if (elapsed > 30000) {
    humTestState = 3;
    humTestStatus = "Провален (изменение <5% за 30 сек)";
    Serial.println("[AHT10] Тест влажности: таймаут");
    sett.reload();
    return;
  }

  float diff = rawHum - humTestStartVal;
  char buf[128];
  snprintf(buf, sizeof(buf), "Тест... Изменение: %+.1f %% (осталось %lu сек)", diff, (unsigned long)((30000 - elapsed) / 1000));
  humTestStatus = buf;

  if (abs(diff) >= 5.0f) {
    humTestState = 2;
    snprintf(buf, sizeof(buf), "Пройден успешно! (Скачок: %+.1f %%)", diff);
    humTestStatus = buf;
    Serial.printf("[AHT10] Тест влажности успешно пройден! Изменение: %+.1f %%\n", diff);
    sett.reload();
  }
}

// --- ДИАГНОСТИКА И ТЕСТ КНОПОК ---
enum Button { BTN_NONE, BTN_RIGHT, BTN_LEFT, BTN_CENTER, BTN_MENU, BTN_BACK };
int liveKeypadAdc = 4095;
int lastSettledAdc = 4095;
String lastPressedBtnName = "Нет (в покое)";
uint32_t lastPressTimeMs = 0;
int totalPressCount = 0;

const char* buttonToName(Button btn) {
  switch (btn) {
    case BTN_CENTER: return "ЦЕНТР (OK)";
    case BTN_MENU:   return "МЕНЮ";
    case BTN_LEFT:   return "ВЛЕВО (◄)";
    case BTN_RIGHT:  return "ВПРАВО (►)";
    case BTN_BACK:   return "НАЗАД (ESC)";
    default:         return "НЕТ (в покое)";
  }
}

// --- МАСТЕР ОБУЧЕНИЯ КНОПОК ---
int btnCalibStep = 0; // 0 - idle, 1 - Center, 2 - Menu, 3 - Left, 4 - Right, 5 - Back, 6 - Done
bool btnWaitingRelease = false;
String btnCalibMsg = "Нажмите «Запустить мастер обучения», чтобы обучить все 5 кнопок.";

void startBtnCalib() {
  btnCalibStep = 1;
  btnWaitingRelease = false;
  btnCalibMsg = "👉 Шаг 1/5: Зажмите кнопку «ЦЕНТР (OK)» и удерживайте...";
}

void cancelBtnCalib() {
  btnCalibStep = 0;
  btnWaitingRelease = false;
  btnCalibMsg = "Обучение отменено пользователем.";
}

void tickBtnCalib() {
  if (btnCalibStep == 0 || btnCalibStep == 6) return;

  static uint32_t lastCalibPoll = 0;
  if (millis() - lastCalibPoll < 25) return;
  lastCalibPoll = millis();

  int live = analogRead(cfg.keypadPin);

  if (btnWaitingRelease) {
    if (live >= 3000) {
      btnWaitingRelease = false;
    }
    return;
  }

  // Если зафиксировано нажатие (уход из зоны покоя < 3000)
  if (live < 2800) {
    static uint32_t stableSince = 0;
    static int lastAdcVal = -1;

    if (abs(live - lastAdcVal) > 30) {
      lastAdcVal = live;
      stableSince = millis();
      return;
    }

    if (millis() - stableSince >= 250) { // 250 мс удержания для надежной фиксации
      if (btnCalibStep == 1) {
        cfg.adcCenter = live;
        prefs.putInt("adcCenter", cfg.adcCenter);
        btnCalibStep = 2;
        btnWaitingRelease = true;
        btnCalibMsg = "✅ ЦЕНТР сохранен (" + String(live) + " ADC). Отпустите и зажмите «МЕНЮ»...";
      } else if (btnCalibStep == 2) {
        cfg.adcMenu = live;
        prefs.putInt("adcMenu", cfg.adcMenu);
        btnCalibStep = 3;
        btnWaitingRelease = true;
        btnCalibMsg = "✅ МЕНЮ сохранено (" + String(live) + " ADC). Отпустите и зажмите «ВЛЕВО»...";
      } else if (btnCalibStep == 3) {
        cfg.adcLeft = live;
        prefs.putInt("adcLeft", cfg.adcLeft);
        btnCalibStep = 4;
        btnWaitingRelease = true;
        btnCalibMsg = "✅ ВЛЕВО сохранено (" + String(live) + " ADC). Отпустите и зажмите «ВПРАВО»...";
      } else if (btnCalibStep == 4) {
        cfg.adcRight = live;
        prefs.putInt("adcRight", cfg.adcRight);
        btnCalibStep = 5;
        btnWaitingRelease = true;
        btnCalibMsg = "✅ ВПРАВО сохранено (" + String(live) + " ADC). Отпустите и зажмите «НАЗАД»...";
      } else if (btnCalibStep == 5) {
        cfg.adcBack = live;
        prefs.putInt("adcBack", cfg.adcBack);
        btnCalibStep = 6;
        btnWaitingRelease = true;
        btnCalibMsg = "🎉 ВСЕ 5 КНОПОК УСПЕШНО ОБУЧЕНЫ И СОХРАНЕНЫ В ПАМЯТЬ!";
      }
    }
  }
}

Button rawValToButton(int raw) {
  if (raw >= 3000) return BTN_NONE; // Покой (3.3V)

  // Проверяем каждую кнопку по сохраненному порогу и допуску
  if (abs(raw - cfg.adcCenter) <= cfg.adcTol) return BTN_CENTER;
  if (abs(raw - cfg.adcMenu)   <= cfg.adcTol) return BTN_MENU;
  if (abs(raw - cfg.adcLeft)   <= cfg.adcTol) return BTN_LEFT;
  if (abs(raw - cfg.adcRight)  <= cfg.adcTol) return BTN_RIGHT;
  if (abs(raw - cfg.adcBack)   <= cfg.adcTol) return BTN_BACK;

  return BTN_NONE;
}

Button readButtonsDebounced() {
  static Button lastStableBtn = BTN_NONE;
  static Button currentRawBtn = BTN_NONE;
  static uint32_t stableTimer = 0;
  static bool btnReleased = true;
  static uint32_t lastPollMs = 0;

  if (millis() < 1500) return BTN_NONE;

  // Неблокирующий опрос АЦП каждые 25 мс на millis() для снижения энергопотребления
  if (millis() - lastPollMs < 25) return BTN_NONE;
  lastPollMs = millis();

  liveKeypadAdc = analogRead(cfg.keypadPin);
  Button sample = rawValToButton(liveKeypadAdc);

  if (sample != currentRawBtn) {
    currentRawBtn = sample;
    stableTimer = millis();
    return BTN_NONE;
  }

  // Фильтр удержания 50 мс
  if (millis() - stableTimer >= 50) {
    if (currentRawBtn == BTN_NONE) {
      btnReleased = true;
      lastStableBtn = BTN_NONE;
    } else if (btnReleased && currentRawBtn != lastStableBtn) {
      btnReleased = false;
      lastStableBtn = currentRawBtn;
      lastSettledAdc = liveKeypadAdc;
      lastPressedBtnName = buttonToName(lastStableBtn);
      lastPressTimeMs = millis();
      totalPressCount++;
      Serial.printf("[KEYPAD] НАЖАТА КНОПКА: %s (ADC: %d, всего: %d)\n", lastPressedBtnName.c_str(), lastSettledAdc, totalPressCount);
      return lastStableBtn;
    }
  }

  return BTN_NONE;
}

// --- WI-FI СКАНИРОВАНИЕ И ПОДКЛЮЧЕНИЕ ---
bool wifiScanning = false;
String wifiScanStatus = "";
String wifiNetworksList = "";
String scannedSsids[15];
int scannedNetworksCount = 0;
int selectedNetworkIndex = -1;

void tickWiFiScan() {
  static uint32_t scanStartMs = 0;
  if (!wifiScanning) {
    scanStartMs = 0;
    return;
  }

  if (scanStartMs == 0) {
    scanStartMs = millis();
  }

  int16_t n = WiFi.scanComplete();
  if (n >= 0) {
    wifiScanning = false;
    scanStartMs = 0;
    scannedNetworksCount = constrain((int)n, 0, 15);
    wifiNetworksList = "";
    for (int i = 0; i < scannedNetworksCount; ++i) {
      if (i > 0) wifiNetworksList += ";";
      scannedSsids[i] = WiFi.SSID(i);
      wifiNetworksList += scannedSsids[i];
      wifiNetworksList += " (";
      wifiNetworksList += String(WiFi.RSSI(i));
      wifiNetworksList += "dBm)";
    }
    WiFi.scanDelete();
    wifiScanStatus = "Найдено сетей: " + String(scannedNetworksCount);
    sett.reload();
  } else if (n == -2) {
    if (millis() - scanStartMs > 10000) {
      wifiScanning = false;
      scanStartMs = 0;
      wifiScanStatus = "Ошибка поиска сетей";
      scannedNetworksCount = 0;
      sett.reload();
    }
  }
}

// 🌐 СОХРАНЕНИЕ И АВТОПОДКЛЮЧЕНИЕ К НЕСКОЛЬКИМ ТОЧКАМ ДОСТУПА (MULTI-AP)
#define MAX_SAVED_APS 5

struct SavedAP {
  char ssid[32] = "";
  char pass[32] = "";
};

SavedAP savedAPs[MAX_SAVED_APS];
int savedApsCount = 0;
int selectedSavedApIndex = 0;

void saveSavedAPs() {
  prefs.putInt("apCount", savedApsCount);
  for (int i = 0; i < MAX_SAVED_APS; i++) {
    String sKey = "apS" + String(i);
    String pKey = "apP" + String(i);
    if (i < savedApsCount) {
      prefs.putString(sKey.c_str(), savedAPs[i].ssid);
      prefs.putString(pKey.c_str(), savedAPs[i].pass);
    } else {
      prefs.remove(sKey.c_str());
      prefs.remove(pKey.c_str());
    }
  }
}

void loadSavedAPs() {
  savedApsCount = prefs.getInt("apCount", 0);
  savedApsCount = constrain(savedApsCount, 0, MAX_SAVED_APS);
  for (int i = 0; i < savedApsCount; i++) {
    String sKey = "apS" + String(i);
    String pKey = "apP" + String(i);
    String s = prefs.getString(sKey.c_str(), "");
    String p = prefs.getString(pKey.c_str(), "");
    snprintf(savedAPs[i].ssid, sizeof(savedAPs[i].ssid), "%s", s.c_str());
    snprintf(savedAPs[i].pass, sizeof(savedAPs[i].pass), "%s", p.c_str());
  }

  // Если NVS еще пуст, но сохранена старая сеть — импортируем её
  if (savedApsCount == 0) {
    String savedSsid = prefs.getString("ssid", "Salomanov");
    String savedPass = prefs.getString("pass", "salomanov");
    if (savedSsid.length() > 0) {
      snprintf(savedAPs[0].ssid, sizeof(savedAPs[0].ssid), "%s", savedSsid.c_str());
      snprintf(savedAPs[0].pass, sizeof(savedAPs[0].pass), "%s", savedPass.c_str());
      savedApsCount = 1;
      saveSavedAPs();
    }
  }
}

bool addOrUpdateAP(const char* ssid, const char* pass) {
  if (!ssid || strlen(ssid) == 0) return false;
  
  for (int i = 0; i < savedApsCount; i++) {
    if (strcmp(savedAPs[i].ssid, ssid) == 0) {
      snprintf(savedAPs[i].pass, sizeof(savedAPs[i].pass), "%s", pass);
      saveSavedAPs();
      return true;
    }
  }

  if (savedApsCount < MAX_SAVED_APS) {
    snprintf(savedAPs[savedApsCount].ssid, sizeof(savedAPs[savedApsCount].ssid), "%s", ssid);
    snprintf(savedAPs[savedApsCount].pass, sizeof(savedAPs[savedApsCount].pass), "%s", pass);
    savedApsCount++;
    saveSavedAPs();
    return true;
  }

  snprintf(savedAPs[MAX_SAVED_APS - 1].ssid, sizeof(savedAPs[MAX_SAVED_APS - 1].ssid), "%s", ssid);
  snprintf(savedAPs[MAX_SAVED_APS - 1].pass, sizeof(savedAPs[MAX_SAVED_APS - 1].pass), "%s", pass);
  saveSavedAPs();
  return true;
}

void removeAP(int index) {
  if (index < 0 || index >= savedApsCount) return;
  for (int i = index; i < savedApsCount - 1; i++) {
    savedAPs[i] = savedAPs[i + 1];
  }
  savedApsCount--;
  saveSavedAPs();
}

void autoConnectSavedAP() {
  if (savedApsCount == 0) {
    WiFiConnector.connect("Salomanov", "salomanov");
    return;
  }

  int n = WiFi.scanNetworks(false, false);
  int bestIndex = -1;
  int maxRssi = -999;

  if (n > 0) {
    for (int i = 0; i < n; i++) {
      String scannedSSID = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      for (int j = 0; j < savedApsCount; j++) {
        if (scannedSSID == String(savedAPs[j].ssid)) {
          if (rssi > maxRssi) {
            maxRssi = rssi;
            bestIndex = j;
          }
        }
      }
    }
    WiFi.scanDelete();
  }

  if (bestIndex >= 0) {
    snprintf(cfg.wifiSsid, sizeof(cfg.wifiSsid), "%s", savedAPs[bestIndex].ssid);
    snprintf(cfg.wifiPass, sizeof(cfg.wifiPass), "%s", savedAPs[bestIndex].pass);
    Serial.printf("[Wi-Fi Multi-AP] Найдена лучшая сеть '%s' (RSSI %d dBm)\n", cfg.wifiSsid, maxRssi);
  } else {
    snprintf(cfg.wifiSsid, sizeof(cfg.wifiSsid), "%s", savedAPs[0].ssid);
    snprintf(cfg.wifiPass, sizeof(cfg.wifiPass), "%s", savedAPs[0].pass);
    Serial.printf("[Wi-Fi Multi-AP] Попытка подключения к первой сохранённой '%s'\n", cfg.wifiSsid);
  }

  prefs.putString("ssid", cfg.wifiSsid);
  prefs.putString("pass", cfg.wifiPass);
  WiFiConnector.connect(cfg.wifiSsid, cfg.wifiPass);
}

enum DisplayMode { DISP_MAIN, DISP_WIFI, DISP_SETTINGS, DISP_CALIB_DS, DISP_CALIB_AHT };
DisplayMode currentDispMode = DISP_MAIN;

// Таймеры
GTimer tempUpdateTimer(MS, 500); // Неблокирующее обновление экрана 2 раза в секунду

void printCyrillicPadded(const char* str, uint8_t width = 16) {
  uint8_t charsPrinted = 0;
  while (*str && charsPrinted < width) {
    uint8_t c = (uint8_t)*str;
    if (c == 0xD0 || c == 0xD1) {
      str++;
      uint8_t next = (uint8_t)*str;
      if (!next) break;

      uint16_t unicode = ((uint16_t)c << 8) | next;
      uint8_t outChar = '?';

      switch (unicode) {
        case 0xD081: outChar = 0xCB; break; // Ё
        case 0xD191: outChar = 0xEB; break; // ё
        case 0xD090: outChar = 'A';  break;
        case 0xD091: outChar = 0x80; break;
        case 0xD092: outChar = 'B';  break;
        case 0xD093: outChar = 0x81; break;
        case 0xD094: outChar = 0x82; break;
        case 0xD095: outChar = 'E';  break;
        case 0xD096: outChar = 0x83; break;
        case 0xD097: outChar = 0x92; break;
        case 0xD098: outChar = 0x84; break;
        case 0xD099: outChar = 0x85; break;
        case 0xD09A: outChar = 'K';  break;
        case 0xD09B: outChar = 0x86; break;
        case 0xD09C: outChar = 'M';  break;
        case 0xD09D: outChar = 'H';  break;
        case 0xD09E: outChar = 'O';  break;
        case 0xD09F: outChar = 0x87; break;
        case 0xD0A0: outChar = 'P';  break;
        case 0xD0A1: outChar = 'C';  break;
        case 0xD0A2: outChar = 'T';  break;
        case 0xD0A3: outChar = 0x93; break;
        case 0xD0A4: outChar = 0x88; break;
        case 0xD0A5: outChar = 'X';  break;
        case 0xD0A6: outChar = 0x89; break;
        case 0xD0A7: outChar = 0x8A; break;
        case 0xD0A8: outChar = 0x8B; break;
        case 0xD0A9: outChar = 0x8C; break;
        case 0xD0AA: outChar = 0x8D; break;
        case 0xD0AB: outChar = 0x8E; break;
        case 0xD0AC: outChar = 'b';  break;
        case 0xD0AD: outChar = 0x8F; break;
        case 0xD0AE: outChar = 0x90; break;
        case 0xD0AF: outChar = 0x91; break;

        case 0xD0B0: outChar = 'a';  break;
        case 0xD0B1: outChar = 0xA0; break;
        case 0xD0B2: outChar = 0xB5; break;
        case 0xD0B3: outChar = 0xA1; break;
        case 0xD0B4: outChar = 0xA2; break;
        case 0xD0B5: outChar = 'e';  break;
        case 0xD0B6: outChar = 0xA3; break;
        case 0xD0B7: outChar = 0xB2; break;
        case 0xD0B8: outChar = 0xA4; break;
        case 0xD0B9: outChar = 0xA5; break;
        case 0xD0BA: outChar = 0xB6; break;
        case 0xD0BB: outChar = 0xA6; break;
        case 0xD0BC: outChar = 0xB7; break;
        case 0xD0BD: outChar = 0xB8; break;
        case 0xD0BE: outChar = 'o';  break;
        case 0xD0BF: outChar = 0xA7; break;
        case 0xD180: outChar = 'p';  break;
        case 0xD181: outChar = 'c';  break;
        case 0xD182: outChar = 0xB9; break;
        case 0xD183: outChar = 0xB3; break;
        case 0xD184: outChar = 0xA8; break;
        case 0xD185: outChar = 'x';  break;
        case 0xD186: outChar = 0xA9; break;
        case 0xD187: outChar = 0xAA; break;
        case 0xD188: outChar = 0xAB; break;
        case 0xD189: outChar = 0xAC; break;
        case 0xD18A: outChar = 0xAD; break;
        case 0xD18B: outChar = 0xAE; break;
        case 0xD18C: outChar = 0xB4; break;
        case 0xD18D: outChar = 0xAF; break;
        case 0xD18E: outChar = 0xB0; break;
        case 0xD18F: outChar = 0xB1; break;
        default: outChar = '?'; break;
      }
      lcd.write(outChar);
      charsPrinted++;
    } else {
      lcd.write(c);
      charsPrinted++;
    }
    str++;
  }
  while (charsPrinted < width) {
    lcd.write(' ');
    charsPrinted++;
  }
}

void printCyrillicChar(uint8_t c) {
  static uint8_t prevByte = 0;
  if (prevByte == 0xD0 || prevByte == 0xD1 || prevByte == 0xC2) {
    uint16_t unicode = ((uint16_t)prevByte << 8) | c;
    prevByte = 0;
    uint8_t outChar = '?';
    switch (unicode) {
      case 0xC2B0: outChar = 0xDF; break; // °
      case 0xD081: outChar = 0xCB; break; // Ё
      case 0xD191: outChar = 0xEB; break; // ё
      case 0xD090: outChar = 'A';  break;
      case 0xD091: outChar = 0x80; break;
      case 0xD092: outChar = 'B';  break;
      case 0xD093: outChar = 0x81; break;
      case 0xD094: outChar = 0x82; break;
      case 0xD095: outChar = 'E';  break;
      case 0xD096: outChar = 0x83; break;
      case 0xD097: outChar = 0x92; break;
      case 0xD098: outChar = 0x84; break;
      case 0xD099: outChar = 0x85; break;
      case 0xD09A: outChar = 'K';  break;
      case 0xD09B: outChar = 0x86; break;
      case 0xD09C: outChar = 'M';  break;
      case 0xD09D: outChar = 'H';  break;
      case 0xD09E: outChar = 'O';  break;
      case 0xD09F: outChar = 0x87; break;
      case 0xD0A0: outChar = 'P';  break;
      case 0xD0A1: outChar = 'C';  break;
      case 0xD0A2: outChar = 'T';  break;
      case 0xD0A3: outChar = 0x93; break;
      case 0xD0A4: outChar = 0x88; break;
      case 0xD0A5: outChar = 'X';  break;
      case 0xD0A6: outChar = 0x89; break;
      case 0xD0A7: outChar = 0x8A; break;
      case 0xD0A8: outChar = 0x8B; break;
      case 0xD0A9: outChar = 0x8C; break;
      case 0xD0AA: outChar = 0x8D; break;
      case 0xD0AB: outChar = 0x8E; break;
      case 0xD0AC: outChar = 'b';  break;
      case 0xD0AD: outChar = 0x8F; break;
      case 0xD0AE: outChar = 0x90; break;
      case 0xD0AF: outChar = 0x91; break;

      case 0xD0B0: outChar = 'a';  break;
      case 0xD0B1: outChar = 0xA0; break;
      case 0xD0B2: outChar = 0xB5; break;
      case 0xD0B3: outChar = 0xA1; break;
      case 0xD0B4: outChar = 0xA2; break;
      case 0xD0B5: outChar = 'e';  break;
      case 0xD0B6: outChar = 0xA3; break;
      case 0xD0B7: outChar = 0xB2; break;
      case 0xD0B8: outChar = 0xA4; break;
      case 0xD0B9: outChar = 0xA5; break;
      case 0xD0BA: outChar = 0xB6; break;
      case 0xD0BB: outChar = 0xA6; break;
      case 0xD0BC: outChar = 0xB7; break;
      case 0xD0BD: outChar = 0xB8; break;
      case 0xD0BE: outChar = 'o';  break;
      case 0xD0BF: outChar = 0xA7; break;
      case 0xD180: outChar = 'p';  break;
      case 0xD181: outChar = 'c';  break;
      case 0xD182: outChar = 0xB9; break;
      case 0xD183: outChar = 0xB3; break;
      case 0xD184: outChar = 0xA8; break;
      case 0xD185: outChar = 'x';  break;
      case 0xD186: outChar = 0xA9; break;
      case 0xD187: outChar = 0xAA; break;
      case 0xD188: outChar = 0xAB; break;
      case 0xD189: outChar = 0xAC; break;
      case 0xD18A: outChar = 0xAD; break;
      case 0xD18B: outChar = 0xAE; break;
      case 0xD18C: outChar = 0xB4; break;
      case 0xD18D: outChar = 0xAF; break;
      case 0xD18E: outChar = 0xB0; break;
      case 0xD18F: outChar = 0xB1; break;
      default: outChar = '?'; break;
    }
    lcd.write(outChar);
  } else if (c == 0xD0 || c == 0xD1 || c == 0xC2) {
    prevByte = c;
  } else {
    lcd.write(c);
  }
}


void initAHT10() {
  Wire.beginTransmission(AHT10_I2C_ADDR);
  Wire.write(0xBA); // Soft reset
  Wire.endTransmission();
  delay(30);

  Wire.beginTransmission(AHT10_I2C_ADDR);
  Wire.write(0xBE); // AHT20/21 init
  Wire.write(0x08);
  Wire.write(0x00);
  Wire.endTransmission();
  delay(15);

  Wire.beginTransmission(AHT10_I2C_ADDR);
  Wire.write(0xE1); // AHT10 init
  Wire.write(0x08);
  Wire.write(0x00);
  Wire.endTransmission();
  delay(15);

  ahtFound = aht.begin(&Wire);
  if (ahtFound) {
    uint8_t status = aht.getStatus();
    Serial.printf("[AHT10] Датчик найден и откалиброван (Adafruit AHTX0, Status: 0x%02X)\n", status);
  } else {
    Serial.println("[AHT10] Датчик не обнаружен на шине I2C (SDA=21, SCL=22)");
  }
}

// 🌡️ ЛОГИКА КАЛИБРОВКИ DS18B20 И AHT10
void startDsCalib() {
  dsCalibState = 1;
  dsCalibStartMs = millis();
  dsCalibStableCount = 0;
  dsCalibPrevIdx = 0;
  dsCalibResult = NAN;
  for (uint8_t i = 0; i < DS_STABLE_COUNT; i++) dsCalibPrev[i] = 0.0f;
  dsCalibStatus = "Запуск: прижмите DS18B20 к телу";
  Serial.println("Автокалибровка DS18B20 запущена");
}

void tickDsAutoCalib(float rawTemp) {
  if (dsCalibState != 1) return;

  char buf[128];
  // 1. Проверяем, что датчик прижат к телу
  if (rawTemp < 35.0f) {
    dsCalibStartMs = millis();
    dsCalibStableCount = 0;
    dsCalibPrevIdx = 0;
    snprintf(buf, sizeof(buf), "Прогрев DS18: %.2f °C (прижмите к телу, нужно >35.0 °C)", rawTemp);
    dsCalibStatus = buf;
    return;
  }

  uint32_t elapsed = millis() - dsCalibStartMs;
  if (elapsed > DS_CALIB_TIMEOUT) {
    dsCalibState = 3;
    dsCalibStableCount = 0;
    dsCalibStatus = "Таймаут: температура не стабилизировалась за 5 мин";
    Serial.println("Автокалибровка DS18: ошибка по таймауту");
    sett.reload();
    return;
  }

  // 2. Проверяем изменение температуры относительно предыдущего замера
  static float lastDsVal = 0.0f;
  float stepDiff = fabs(rawTemp - lastDsVal);
  lastDsVal = rawTemp;

  dsCalibPrev[dsCalibPrevIdx] = rawTemp;
  dsCalibPrevIdx = (dsCalibPrevIdx + 1) % DS_STABLE_COUNT;

  // Если температура растет или меняется больше чем на 0.04 °C — сбрасываем счетчик стабильности!
  if (stepDiff > 0.04f) {
    dsCalibStableCount = 0;
    snprintf(buf, sizeof(buf), "Прогрев тела: %.2f °C (температура меняется, ждем плато)", rawTemp);
    dsCalibStatus = buf;
    return;
  }

  // Температура не меняется — увеличиваем счетчик стабильных секунд
  dsCalibStableCount++;

  if (dsCalibStableCount < DS_STABLE_COUNT) {
    snprintf(buf, sizeof(buf), "Неизменная T: %.2f °C (держится %d из 60 сек)", rawTemp, dsCalibStableCount);
    dsCalibStatus = buf;
    return;
  }

  // Температура держалась неизменной ровно 60 секунд!
  float minVal = dsCalibPrev[0], maxVal = dsCalibPrev[0], avg = 0;
  for (uint8_t i = 0; i < DS_STABLE_COUNT; i++) {
    if (dsCalibPrev[i] < minVal) minVal = dsCalibPrev[i];
    if (dsCalibPrev[i] > maxVal) maxVal = dsCalibPrev[i];
    avg += dsCalibPrev[i];
  }
  avg /= DS_STABLE_COUNT;

  if ((maxVal - minVal) <= DS_STABLE_DELTA) {
    dsCalibResult = DS_BODY_TARGET - avg;
    calDsOffset = dsCalibResult;
    dsCalibState = 2;
    dsCalibStableCount = 0;
    snprintf(buf, sizeof(buf), "Готово! Неизменная T (60с): %.2f °C, поправка DS18: %+.2f °C", avg, calDsOffset);
    dsCalibStatus = buf;
    prefs.putFloat("dsOffset", calDsOffset);
    Serial.printf("Автокалибровка DS18 успешно завершена. Ср: %.2f °C, Поправка: %+.2f °C\n", avg, calDsOffset);
    sett.reload();
  } else {
    dsCalibStableCount = 30;
  }
}

void startAhtCalib() {
  ahtCalibState = 1;
  ahtCalibStartMs = millis();
  ahtCalibStableCount = 0;
  ahtCalibPrevIdx = 0;
  ahtCalibResult = NAN;
  for (uint8_t i = 0; i < DS_STABLE_COUNT; i++) {
    ahtCalibPrevDS[i] = 0.0f;
    ahtCalibPrevAHT[i] = 0.0f;
  }
  ahtCalibStatus = "Запуск: положите AHT10 и DS18B20 рядом";
  Serial.println("Автокалибровка AHT10 по DS18B20 запущена");
}

void tickAhtAutoCalib(float rawAhtTemp) {
  if (ahtCalibState != 1) return;

  char buf[128];
  if (!ds18Ok || isnan(tempDS18)) {
    ahtCalibState = 3;
    ahtCalibStatus = "Ошибка: эталонный DS18B20 отключен";
    Serial.println("Автокалибровка AHT10: ошибка - DS18B20 не активен");
    sett.reload();
    return;
  }

  uint32_t elapsed = millis() - ahtCalibStartMs;
  if (elapsed > DS_CALIB_TIMEOUT) {
    ahtCalibState = 3;
    ahtCalibStatus = "Таймаут: температура обоих датчиков не стабилизировалась за 5 мин";
    Serial.println("Автокалибровка AHT10: ошибка по таймауту");
    sett.reload();
    return;
  }

  static float lastAhtDS = 0.0f;
  static float lastAhtVal = 0.0f;
  float diffDS = fabs(tempDS18 - lastAhtDS);
  float diffAHT = fabs(rawAhtTemp - lastAhtVal);
  lastAhtDS = tempDS18;
  lastAhtVal = rawAhtTemp;

  ahtCalibPrevDS[ahtCalibPrevIdx] = tempDS18;
  ahtCalibPrevAHT[ahtCalibPrevIdx] = rawAhtTemp;
  ahtCalibPrevIdx = (ahtCalibPrevIdx + 1) % DS_STABLE_COUNT;

  // Если ХОТЯ БЫ ОДИН датчик меняется (> 0.04°C за шаг) — сбрасываем 60-секундный таймер стабильности!
  if (diffDS > 0.04f || diffAHT > 0.04f) {
    ahtCalibStableCount = 0;
    snprintf(buf, sizeof(buf), "Датчики меняются: DS %.2f | AHT %.2f (ждем стабилизации обоих)", tempDS18, rawAhtTemp);
    ahtCalibStatus = buf;
    return;
  }

  // Оба датчика неподвижны — наращиваем секунды стабильности!
  ahtCalibStableCount++;

  if (ahtCalibStableCount < DS_STABLE_COUNT) {
    snprintf(buf, sizeof(buf), "Оба датчика не меняются: DS %.2f | AHT %.2f (%d из 60 сек)", tempDS18, rawAhtTemp, ahtCalibStableCount);
    ahtCalibStatus = buf;
    return;
  }

  // Оба датчика держались неизменными целую минуту (60 секунд)!
  float minDS = ahtCalibPrevDS[0], maxDS = ahtCalibPrevDS[0], avgDS = 0;
  float minAHT = ahtCalibPrevAHT[0], maxAHT = ahtCalibPrevAHT[0], avgAHT = 0;
  for (uint8_t i = 0; i < DS_STABLE_COUNT; i++) {
    if (ahtCalibPrevDS[i] < minDS) minDS = ahtCalibPrevDS[i];
    if (ahtCalibPrevDS[i] > maxDS) maxDS = ahtCalibPrevDS[i];
    avgDS += ahtCalibPrevDS[i];

    if (ahtCalibPrevAHT[i] < minAHT) minAHT = ahtCalibPrevAHT[i];
    if (ahtCalibPrevAHT[i] > maxAHT) maxAHT = ahtCalibPrevAHT[i];
    avgAHT += ahtCalibPrevAHT[i];
  }
  avgDS /= DS_STABLE_COUNT;
  avgAHT /= DS_STABLE_COUNT;

  if ((maxDS - minDS) <= DS_STABLE_DELTA && (maxAHT - minAHT) <= DS_STABLE_DELTA) {
    ahtCalibResult = avgDS - avgAHT;
    cfg.tempOffset = ahtCalibResult;
    ahtCalibState = 2;
    ahtCalibStableCount = 0;
    snprintf(buf, sizeof(buf), "Готово! Неизменная T за 60с (DS: %.2f, AHT: %.2f), поправка AHT10: %+.2f °C", avgDS, avgAHT, cfg.tempOffset);
    ahtCalibStatus = buf;
    prefs.putFloat("offset", cfg.tempOffset);
    Serial.printf("Автокалибровка AHT10 по DS18B20 успешно завершена. Новая поправка: %+.2f °C\n", cfg.tempOffset);
    sett.reload();
  } else {
    ahtCalibStableCount = 30;
  }
}

void tickAHT10() {
  static uint32_t tmr = 0;
  if (millis() - tmr < 1200) return;
  tmr = millis();

  if (ahtFound) {
    sensors_event_t humidityEvent, tempEvent;
    if (aht.getEvent(&humidityEvent, &tempEvent)) {
      float rawTemp = tempEvent.temperature;
      float rawHum = humidityEvent.relative_humidity;

      lastRawTemp = rawTemp;
      lastRawHum = rawHum;

      // Если датчик вернул 0.0% влажности (не откалиброван или завис)
      if (rawHum <= 0.01f) {
        static uint32_t lastZeroHumRetry = 0;
        if (millis() - lastZeroHumRetry > 3000) {
          lastZeroHumRetry = millis();
          Wire.beginTransmission(AHT10_I2C_ADDR);
          Wire.write(0xBE);
          Wire.write(0x08);
          Wire.write(0x00);
          Wire.endTransmission();
          delay(10);
          Wire.beginTransmission(AHT10_I2C_ADDR);
          Wire.write(0xE1);
          Wire.write(0x08);
          Wire.write(0x00);
          Wire.endTransmission();
        }
      }

      if (!isnan(rawTemp) && !isnan(rawHum) && rawHum >= 0.0f && rawHum <= 100.0f) {
        climateOk = true;

        static bool firstReadAht = true;
        if (firstReadAht) {
          firstReadAht = false;
          ahtTempFilter.setCoef(1.0f);
          ahtTempFilter.filtered(rawTemp);
          ahtTempFilter.setCoef(0.15f);

          ahtHumFilter.setCoef(1.0f);
          ahtHumFilter.filtered(rawHum);
          ahtHumFilter.setCoef(0.15f);
        }

        float filteredRawAhtTemp = ahtTempFilter.filtered(rawTemp);
        float filteredRawHum = ahtHumFilter.filtered(rawHum);

        currentHum = constrain(filteredRawHum + cfg.humOffset, 0.0f, 100.0f);
        currentTemp = filteredRawAhtTemp + cfg.tempOffset;

        if (currentHum < 0.0f) currentHum = 0.0f;
        if (currentHum > 100.0f) currentHum = 100.0f;
        currentHum = roundf(currentHum * 10.0f) / 10.0f;
        currentTemp = roundf(currentTemp * 10.0f) / 10.0f;

        // Автокалибровка AHT10 по DS18B20: передаем отфильтрованные сглаженные данные!
        tickAhtAutoCalib(filteredRawAhtTemp);

        // Экспресс-тест влажности (дыханием)
        tickHumTest(rawHum);
      }
    } else {
      climateOk = false;
      ahtFound = false;
      Serial.println("[AHT10] Сбой чтения I2C, запуск переподключения...");
    }
  } else {
    initAHT10();
  }
}

void tickDS18() {
  uint8_t dsState = ds18.tick();
  if (dsState == DS18_READY) {
    float raw = ds18.getTemp();

    static bool firstReadDs = true;
    if (firstReadDs) {
      firstReadDs = false;
      tempFilter.setCoef(1.0f);
      tempFilter.filtered(raw);
      tempFilter.setCoef(0.15f);
    }

    float filteredRawDs = tempFilter.filtered(raw);
    tempDS18 = filteredRawDs + calDsOffset;
    tempDS18 = roundf(tempDS18 * 10.0f) / 10.0f;
    ds18Ok = true;

    // Передаем отфильтрованное значение без шума квантования в автокалибровку!
    tickDsAutoCalib(filteredRawDs);
  } else if (dsState == DS18_ERROR) {
    static uint32_t lastDsErrorRecovery = 0;
    if (millis() - lastDsErrorRecovery > 2000) {
      lastDsErrorRecovery = millis();
      ds18.reset();
      ds18.requestTemp();
    }
    ds18Ok = false;
  }
}

// 📟 СТРУКТУРА ЭКРАННОГО МЕНЮ НА БАЗЕ GYVERMENU
void buildLcdMenu(gm::Builder& b) {
  // Подменю 1: Датчики и экран
  b.Page(1, "Датчики", [](gm::Builder& b) {
    b.Select("Экран", &cfg.dispTempSource, "AHT10;DS18;Оба;Ср.", [](uint8_t n, const char* str, uint8_t len) {
      prefs.putInt("tempSrc", cfg.dispTempSource);
      updateDisplayWindow();
    });
    b.ValueFloat("Сдвиг T", &cfg.tempOffset, -10.0f, 10.0f, 0.1f, 1, "C", [](float v) {
      prefs.putFloat("offset", cfg.tempOffset);
    });
    b.ValueFloat("Сдвиг H", &cfg.humOffset, -20.0f, 20.0f, 0.5f, 1, "%", [](float v) {
      prefs.putFloat("humOffset", cfg.humOffset);
    });
    b.ValueFloat("Сдвиг DS", &calDsOffset, -10.0f, 10.0f, 0.1f, 1, "C", [](float v) {
      prefs.putFloat("dsOffset", calDsOffset);
    });
  });

  // Подменю 2: Автокалибровка
  b.Page(2, "Калибровка", [](gm::Builder& b) {
    b.Button("Кал. DS (тело)", []() {
      currentDispMode = DISP_CALIB_DS;
      startDsCalib();
      updateDisplayWindow();
    });
    b.Button("Кал. AHT по DS", []() {
      currentDispMode = DISP_CALIB_AHT;
      startAhtCalib();
      updateDisplayWindow();
    });
  });

  // Подменю 3: Wi-Fi и Система
  b.Page(3, "Инфо/Сеть", [](gm::Builder& b) {
    if (WiFi.status() == WL_CONNECTED) {
      String ip = WiFi.localIP().toString();
      b.ValueStr("IP", ip.c_str());
      char rssiBuf[12];
      snprintf(rssiBuf, sizeof(rssiBuf), "%d dBm", WiFi.RSSI());
      b.ValueStr("Сигнал", rssiBuf);
    } else {
      b.ValueStr("Wi-Fi", "AP:Incubator");
    }
    char uptimeBuf[12];
    uint32_t sec = millis() / 1000;
    snprintf(uptimeBuf, sizeof(uptimeBuf), "%02u:%02u:%02u", sec / 3600, (sec % 3600) / 60, sec % 60);
    b.ValueStr("Аптайм", uptimeBuf);
  });

  // Кнопка возврата на главный экран
  b.Button("< Главный экран", []() {
    currentDispMode = DISP_MAIN;
    updateDisplayWindow();
  });
}

// ⏰ НАСТРОЙКА СИНХРОНИЗАЦИИ ЧАСОВОГО ПОЯСА GMT+5
void setupNTPTime() {
  configTime(cfg.gmtOffsetHours * 3600, 0, "pool.ntp.org", "time.nist.gov", "ru.pool.ntp.org");
  Serial.printf("[NTP] Часовой пояс установлен: GMT+%d\n", cfg.gmtOffsetHours);
}

bool otaInProgress = false;

void showOTAProgressLCD(int percent) {
  static int lastPct = -1;
  if (percent == lastPct) return;
  lastPct = percent;

  char bar[11];
  int filled = (percent * 10) / 100;
  filled = constrain(filled, 0, 10);
  for (int i = 0; i < 10; i++) {
    bar[i] = (i < filled) ? '=' : ' ';
  }
  bar[10] = '\0';

  char line2[17];
  snprintf(line2, sizeof(line2), "[%s] %3d%%", bar, percent);

  lcd.setCursor(0, 0);
  printCyrillicPadded("Обновление ПО...", 16);
  lcd.setCursor(0, 1);
  printCyrillicPadded(line2, 16);
}

void setupOTAOverNetwork() {
  ArduinoOTA.setHostname("Sushka-Incubator");
  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    Serial.println("\n[OTA Network] Начало прошивки по сети...");
    lcd.clear();
    showOTAProgressLCD(0);
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    int pct = (total > 0) ? (progress * 100) / total : 0;
    Serial.printf("[OTA] Прогресс: %u%%\r", pct);
    showOTAProgressLCD(pct);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA Network] Прошивка успешно завершена!");
    lcd.setCursor(0, 0);
    printCyrillicPadded("Прошивка OK!", 16);
    lcd.setCursor(0, 1);
    printCyrillicPadded("Перезагрузка...", 16);
    delay(500);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    Serial.printf("[OTA] Ошибка: %u\n", error);
    lcd.setCursor(0, 0);
    printCyrillicPadded("Сбой прошивки!", 16);
    char buf[17];
    snprintf(buf, sizeof(buf), "Код ошибки: %u", error);
    lcd.setCursor(0, 1);
    printCyrillicPadded(buf, 16);
  });
  ArduinoOTA.begin();
  Serial.println("[OTA Network] Служба прошивки по воздуху (OTA) успешно активна.");
}

// --- ЭНЕРГОСБЕРЕЖЕНИЕ WI-FI (АВТООТКЛЮЧЕНИЕ ЧЕРЕЗ 10 МИНУТ БЕЗДЕЙСТВИЯ) ---
#define WIFI_INACTIVITY_TIMEOUT (10 * 60 * 1000UL) // 10 минут
bool wifiSleeping = false;
uint32_t lastActivityMs = 0;

void setupWiFiIncubatorStyle();

void sleepWiFi() {
  if (wifiSleeping) return;
  wifiSleeping = true;
  Serial.println("\n[Wi-Fi Power] 10 минут бездействия: Wi-Fi модуль отключен для энергосбережения.");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  updateDisplayWindow();
}

void wakeUpWiFi() {
  lastActivityMs = millis();
  if (!wifiSleeping) return;
  wifiSleeping = false;
  Serial.println("\n[Wi-Fi Power] Нажата кнопка: пробуждение Wi-Fi модуля...");
  esp_wifi_start();
  setupWiFiIncubatorStyle();
  setupOTAOverNetwork();
  sett.reload();
  updateDisplayWindow();
}

void tickWiFiInactivity() {
  if (!wifiSleeping && (millis() - lastActivityMs >= WIFI_INACTIVITY_TIMEOUT)) {
    sleepWiFi();
  }
}

// 🌐 WI-FI СЕТЬ ИНКУБАТОРА С АВТО-СИНХРОНИЗАЦИЕЙ NTP ВРЕМЕНИ GMT+5
void setupWiFiIncubatorStyle() {
  WiFiConnector.setName("Incubator");
  WiFiConnector.setPass("12345678");

  WiFiConnector.onConnect([]() {
    Serial.print("[Wi-Fi] Подключен к роутеру! IP: ");
    Serial.println(WiFi.localIP());
    // Отключаем SoftAP точку доступа для снижения энергопотребления и нагрева
    WiFi.enableAP(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    Serial.println("[Wi-Fi] Точка AP 'Incubator' отключена, активен режим энергосбережения STA.");
    setupNTPTime();
  });

  WiFiConnector.onError([]() {
    Serial.print("[Wi-Fi] Ошибка подключения к роутеру. Включаем точку AP: ");
    WiFi.mode(WIFI_AP_STA);
    WiFi.enableAP(true);
    WiFi.softAP("Incubator", "12345678");
    Serial.println(WiFi.softAPIP());
  });

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(true);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  autoConnectSavedAP();
}

// 📖 СБОРКА WEB ИНТЕРФЕЙСА
void buildSettingsUI(sets::Builder& b) {
  lastActivityMs = millis(); // Продлеваем активность Wi-Fi на 10 минут при каждом запросе из браузера

  // 1. ДАШБОРД: КЛИМАТ И ТЕКУЩИЕ ПОКАЗАНИЯ
  {
    sets::Group g(b, "📊 Мониторинг микроклимата");
    {
      sets::Row r(b, "Статус сенсоров", sets::DivType::Line);
      b.LED("DS18B20 (Эталон)", ds18Ok);
      b.LED("AHT10 (Климат)", ahtFound);
    }
    if (climateOk && !isnan(currentTemp)) {
      b.Label("Температура AHT10", String(currentTemp, 1) + " °C");
      b.Label("Влажность AHT10", String(currentHum, 1) + " % (Сырая: " + String(lastRawHum, 1) + " %)");
    } else {
      b.Label("AHT10", "Ошибка чтения датчика");
    }
    if (ds18Ok && !isnan(tempDS18)) {
      b.Label("Эталон DS18B20", String(tempDS18, 1) + " °C");
    } else {
      b.Label("Эталон DS18B20", "Не подключен / Ошибка");
    }

    if (b.Select("🌡️ Датчик для LCD экрана", "AHT10 (Микроклимат);DS18B20 (Эталон);Оба датчика (AHT + DS18 + Влажность);Среднее арифметическое (AHT + DS18)", &cfg.dispTempSource)) {
      prefs.putInt("tempSrc", cfg.dispTempSource);
      updateDisplayWindow();
      b.reload();
    }
  }

  // 2. ДАШБОРД: БЫСТРЫЙ СТАТУС УСТРОЙСТВА
  {
    sets::Group g(b, "⚡ Статус устройства");
    uint32_t uptimeSec = millis() / 1000;
    uint32_t upMin = (uptimeSec / 60) % 60;
    uint32_t upHour = (uptimeSec / 3600);

    float espChipTemp = temperatureRead();
    String tempStatus = (espChipTemp < 55.0f) ? " (Норма)" : (espChipTemp < 70.0f ? " (Теплый)" : " (Горячий!)");

    {
      sets::Row r(b, "Сеть и питание", sets::DivType::Line);
      if (WiFi.status() == WL_CONNECTED) {
        b.Label("Wi-Fi IP", WiFi.localIP().toString() + " (" + String(WiFi.RSSI()) + " dBm)");
      } else {
        b.Label("Wi-Fi AP", WiFi.softAPIP().toString() + " (Incubator)");
      }
      b.Label("Uptime", String(upHour) + "ч " + String(upMin) + "м");
    }

    {
      sets::Row r(b, "Система", sets::DivType::Line);
      b.Label("ESP32 Core", String(espChipTemp, 1) + " °C" + tempStatus);
      b.Label("CPU", String(ESP.getCpuFreqMHz()) + " МГц");
    }
  }

  // ================= ВЛОЖЕННЫЕ МЕНЮ НАСТРОЕК =================

  // 3. МЕНЮ: КАЛИБРОВКА ДАТЧИКОВ
  {
    sets::Menu m(b, "🌡️ Калибровка датчиков");

    {
      sets::Group g(b, "Эталонный термометр DS18B20 (GPIO 16)");
      if (b.Slider("Поправка DS18B20, °C", -10.0f, 10.0f, 0.01f, " °C", &calDsOffset)) {
        prefs.putFloat("dsOffset", calDsOffset);
      }
      b.Paragraph("Статус автокалибровки", dsCalibStatus);
      if (!isnan(dsCalibResult)) {
        b.Label("Вычисленная поправка", String(dsCalibResult, 2) + " °C");
      }
      if (b.Button("🎯 Калибровать DS18B20 по телу (36.6 °C)")) {
        startDsCalib();
        b.reload();
      }
    }

    {
      sets::Group g(b, "Датчик микроклимата AHT10 (I2C)");
      if (b.Slider("Поправка температуры, °C", -10.0f, 10.0f, 0.01f, " °C", &cfg.tempOffset)) {
        prefs.putFloat("offset", cfg.tempOffset);
      }
      if (b.Slider("Поправка влажности, %", -20.0f, 20.0f, 1.0f, " %", &cfg.humOffset)) {
        prefs.putFloat("humOffset", cfg.humOffset);
      }
      b.Paragraph("Статус автокалибровки", ahtCalibStatus);
      if (!isnan(ahtCalibResult)) {
        b.Label("Вычисленная поправка", String(ahtCalibResult, 2) + " °C");
      }
      if (b.Button("🎯 Автокалибровать AHT10 по DS18B20")) {
        startAhtCalib();
        b.reload();
      }
    }

    {
      sets::Group g(b, "Экспресс-тест влажности");
      b.Paragraph("Тест дыханием", humTestStatus);
      if (b.Button("🌬️ Запустить тест влажности (дыханием)")) {
        startHumTest();
        b.reload();
      }
    }
  }

  // 4. МЕНЮ: КЛАВИАТУРА И КНОПКИ
  {
    sets::Menu m(b, "⌨️ Настройка клавиатуры и кнопок");

    {
      sets::Group g(b, "Текущее состояние");
      liveKeypadAdc = analogRead(cfg.keypadPin);
      uint32_t liveMv = analogReadMilliVolts(cfg.keypadPin);
      Button currentLiveBtn = rawValToButton(liveKeypadAdc);

      {
        sets::Row r(b, "Сигнал на входе", sets::DivType::Line);
        b.Label("Пин", "GPIO " + String(cfg.keypadPin));
        b.Label("Живой АЦП", String(liveKeypadAdc) + " (" + String(liveMv) + " мВ)");
      }
      {
        sets::Row r(b, "Кнопка", sets::DivType::Line);
        b.Label("Распознано", buttonToName(currentLiveBtn));
        b.Label("Последнее", lastPressedBtnName + " (" + String(lastSettledAdc) + ")");
      }
    }

    {
      sets::Group g(b, "Мастер автообучения 5 кнопок");
      b.Paragraph("Статус мастера", btnCalibMsg);
      if (btnCalibStep == 0 || btnCalibStep == 6) {
        if (b.Button("🎯 Запустить мастер обучения кнопок")) {
          startBtnCalib();
          b.reload();
        }
      } else {
        if (b.Button("❌ Отменить обучение", sets::Colors::Red)) {
          cancelBtnCalib();
          b.reload();
        }
      }
    }

    {
      sets::Group g(b, "Быстрая запись АЦП для отдельной кнопки");
      b.Label("Инструкция", "Зажмите нужную кнопку и нажмите кнопку записи:");
      {
        sets::Buttons btns(b);
        if (b.Button("Записать ЦЕНТР")) {
          cfg.adcCenter = liveKeypadAdc;
          prefs.putInt("adcCenter", cfg.adcCenter);
          b.reload();
        }
        if (b.Button("Записать МЕНЮ")) {
          cfg.adcMenu = liveKeypadAdc;
          prefs.putInt("adcMenu", cfg.adcMenu);
          b.reload();
        }
        if (b.Button("Записать ВЛЕВО")) {
          cfg.adcLeft = liveKeypadAdc;
          prefs.putInt("adcLeft", cfg.adcLeft);
          b.reload();
        }
      }
      {
        sets::Buttons btns(b);
        if (b.Button("Записать ВПРАВО")) {
          cfg.adcRight = liveKeypadAdc;
          prefs.putInt("adcRight", cfg.adcRight);
          b.reload();
        }
        if (b.Button("Записать НАЗАД")) {
          cfg.adcBack = liveKeypadAdc;
          prefs.putInt("adcBack", cfg.adcBack);
          b.reload();
        }
      }
    }

    {
      sets::Group g(b, "Ручная подстройка порогов АЦП");
      if (b.Slider("ЦЕНТР (OK) АЦП", 0, 3500, 1, "", &cfg.adcCenter)) prefs.putInt("adcCenter", cfg.adcCenter);
      if (b.Slider("МЕНЮ АЦП", 0, 3500, 1, "", &cfg.adcMenu)) prefs.putInt("adcMenu", cfg.adcMenu);
      if (b.Slider("ВЛЕВО (◄) АЦП", 0, 3500, 1, "", &cfg.adcLeft)) prefs.putInt("adcLeft", cfg.adcLeft);
      if (b.Slider("ВПРАВО (►) АЦП", 0, 3500, 1, "", &cfg.adcRight)) prefs.putInt("adcRight", cfg.adcRight);
      if (b.Slider("НАЗАД (ESC) АЦП", 0, 3500, 1, "", &cfg.adcBack)) prefs.putInt("adcBack", cfg.adcBack);
      if (b.Slider("Допуск распознавания (± ADC)", 15, 250, 1, "", &cfg.adcTol)) prefs.putInt("adcTol", cfg.adcTol);

      int pinChoice = (cfg.keypadPin == 35) ? 0 : (cfg.keypadPin == 32 ? 1 : (cfg.keypadPin == 33 ? 2 : (cfg.keypadPin == 34 ? 3 : (cfg.keypadPin == 36 ? 4 : 5))));
      if (b.Select("Пин клавиатуры", "GPIO 35;GPIO 32;GPIO 33;GPIO 34;GPIO 36 (VP);GPIO 39 (VN)", &pinChoice)) {
        const int pinsList[] = {35, 32, 33, 34, 36, 39};
        if (pinChoice >= 0 && pinChoice < 6) {
          cfg.keypadPin = pinsList[pinChoice];
          prefs.putInt("keypadPin", cfg.keypadPin);
        }
        b.reload();
      }

      if (b.Button("🔄 Сбросить калибровки по умолчанию")) {
        cfg.adcCenter = 248;
        cfg.adcMenu = 372;
        cfg.adcLeft = 682;
        cfg.adcRight = 1191;
        cfg.adcBack = 1985;
        cfg.adcTol = 70;
        cfg.keypadPin = 35;
        prefs.putInt("adcCenter", cfg.adcCenter);
        prefs.putInt("adcMenu", cfg.adcMenu);
        prefs.putInt("adcLeft", cfg.adcLeft);
        prefs.putInt("adcRight", cfg.adcRight);
        prefs.putInt("adcBack", cfg.adcBack);
        prefs.putInt("adcTol", cfg.adcTol);
        prefs.putInt("keypadPin", cfg.keypadPin);
        b.reload();
      }
    }
  }

  // 5. МЕНЮ: УПРАВЛЕНИЕ WI-FI
  {
    sets::Menu m(b, "📶 Настройки сети Wi-Fi");

    {
      sets::Group g(b, "Сохранённые сети (" + String(savedApsCount) + "/" + String(MAX_SAVED_APS) + ")");
      if (savedApsCount > 0) {
        String savedApListStr = "";
        for (int i = 0; i < savedApsCount; i++) {
          if (i > 0) savedApListStr += ";";
          savedApListStr += String(savedAPs[i].ssid);
          if (strcmp(cfg.wifiSsid, savedAPs[i].ssid) == 0 && WiFi.status() == WL_CONNECTED) {
            savedApListStr += " (Онлайн)";
          }
        }

        selectedSavedApIndex = constrain(selectedSavedApIndex, 0, savedApsCount - 1);
        if (b.Select("Выбрать сохранённую сеть", savedApListStr, &selectedSavedApIndex)) {
          selectedSavedApIndex = constrain(selectedSavedApIndex, 0, savedApsCount - 1);
          snprintf(cfg.wifiSsid, sizeof(cfg.wifiSsid), "%s", savedAPs[selectedSavedApIndex].ssid);
          snprintf(cfg.wifiPass, sizeof(cfg.wifiPass), "%s", savedAPs[selectedSavedApIndex].pass);
          b.reload();
        }

        {
          sets::Buttons btns(b);
          if (b.Button("🚀 Подключиться")) {
            selectedSavedApIndex = constrain(selectedSavedApIndex, 0, savedApsCount - 1);
            snprintf(cfg.wifiSsid, sizeof(cfg.wifiSsid), "%s", savedAPs[selectedSavedApIndex].ssid);
            snprintf(cfg.wifiPass, sizeof(cfg.wifiPass), "%s", savedAPs[selectedSavedApIndex].pass);
            prefs.putString("ssid", cfg.wifiSsid);
            prefs.putString("pass", cfg.wifiPass);
            WiFiConnector.connect(cfg.wifiSsid, cfg.wifiPass);
            b.reload();
          }
          if (b.Button("🔄 Автовыбор лучшей сети")) {
            autoConnectSavedAP();
            b.reload();
          }
        }

        b.Label("Список профилей", "Управление профилями:");
        for (int i = 0; i < savedApsCount; i++) {
          String rowTitle = "#" + String(i + 1) + " " + String(savedAPs[i].ssid);
          if (strcmp(cfg.wifiSsid, savedAPs[i].ssid) == 0 && WiFi.status() == WL_CONNECTED) {
            rowTitle += " (Активна)";
          }
          if (b.beginRow(rowTitle, sets::DivType::Line)) {
            if (b.Button("Подключить")) {
              snprintf(cfg.wifiSsid, sizeof(cfg.wifiSsid), "%s", savedAPs[i].ssid);
              snprintf(cfg.wifiPass, sizeof(cfg.wifiPass), "%s", savedAPs[i].pass);
              prefs.putString("ssid", cfg.wifiSsid);
              prefs.putString("pass", cfg.wifiPass);
              WiFiConnector.connect(cfg.wifiSsid, cfg.wifiPass);
              b.reload();
            }
            if (b.Button("Удалить", sets::Colors::Red)) {
              removeAP(i);
              b.reload();
            }
            b.endRow();
          }
        }
      } else {
        b.Label("Сохранённые сети", "Список пуст");
      }
    }

    {
      sets::Group g(b, "Подключение новой сети");
      if (b.Button("🔍 Сканировать Wi-Fi сети")) {
        int16_t res = WiFi.scanComplete();
        if (res == -1) {
          wifiScanning = true;
          wifiScanStatus = "Сканирование...";
        } else {
          res = WiFi.scanNetworks(true, false, true, 150);
          if (res == -2) {
            wifiScanStatus = "Ошибка поиска сетей";
            wifiScanning = false;
          } else {
            wifiScanning = true;
            wifiScanStatus = "Сканирование...";
          }
        }
        selectedNetworkIndex = -1;
        b.reload();
      }
      if (wifiScanStatus.length() > 0) {
        b.Label("Статус поиска", wifiScanStatus);
      }
      if (scannedNetworksCount > 0) {
        if (b.Select("Выбрать сеть из списка", wifiNetworksList, &selectedNetworkIndex)) {
          if (selectedNetworkIndex >= 0 && selectedNetworkIndex < scannedNetworksCount) {
            snprintf(cfg.wifiSsid, sizeof(cfg.wifiSsid), "%s", scannedSsids[selectedNetworkIndex].c_str());
            b.reload();
          }
        }
      }
      b.Input("Имя Wi-Fi (SSID)", cfg.wifiSsid);
      b.Pass("Пароль Wi-Fi", cfg.wifiPass);
      {
        sets::Buttons btns(b);
        if (b.Button("💾 Сохранить в память")) {
          addOrUpdateAP(cfg.wifiSsid, cfg.wifiPass);
          b.reload();
        }
        if (b.Button("🚀 Сохранить и подключить")) {
          addOrUpdateAP(cfg.wifiSsid, cfg.wifiPass);
          prefs.putString("ssid", cfg.wifiSsid);
          prefs.putString("pass", cfg.wifiPass);
          WiFiConnector.connect(cfg.wifiSsid, cfg.wifiPass);
        }
      }
    }
  }

  // 6. МЕНЮ: СИСТЕМА И ДИАГНОСТИКА
  {
    sets::Menu m(b, "⚡ Система и диагностика");

    {
      sets::Group g(b, "Время и часовой пояс");
      if (b.Slider("Часовой пояс (GMT)", -12, 14, 1, " ч", &cfg.gmtOffsetHours)) {
        prefs.putInt("gmt", cfg.gmtOffsetHours);
        setupNTPTime();
      }
    }

    {
      sets::Group g(b, "Процессор и питание");
      int freqChoice = (ESP.getCpuFreqMHz() == 240) ? 0 : (ESP.getCpuFreqMHz() == 160 ? 1 : 2);
      if (b.Select("Режим энергопотребления / Частота CPU", "240 МГц (Максимум);160 МГц (Оптимально / Прохладный);80 МГц (Энергосбережение)", &freqChoice)) {
        if (freqChoice == 0) setCpuFrequencyMhz(240);
        else if (freqChoice == 1) setCpuFrequencyMhz(160);
        else if (freqChoice == 2) setCpuFrequencyMhz(80);
        prefs.putInt("cpuFreq", freqChoice);
        b.reload();
      }

      uint32_t elapsedMs = millis() - lastActivityMs;
      uint32_t remainSec = (elapsedMs < WIFI_INACTIVITY_TIMEOUT) ? (WIFI_INACTIVITY_TIMEOUT - elapsedMs) / 1000 : 0;
      b.Label("Автоотключение Wi-Fi", "Через " + String(remainSec / 60) + " мин " + String(remainSec % 60) + " сек (при бездействии)");
    }

    {
      sets::Group g(b, "Память RAM (Куча)");
      {
        sets::Row r(b, "Heap Memory", sets::DivType::Line);
        b.Label("Свободно", String(ESP.getFreeHeap() / 1024) + " КБ");
        b.Label("Минимум", String(ESP.getMinFreeHeap() / 1024) + " КБ");
      }
    }

    {
      sets::Group g(b, "🔍 Сканер аналоговых входов (ADC1)");
      {
        sets::Row r(b, "GPIO 32 - 34", sets::DivType::Line);
        b.Label("GPIO 32", String(analogRead(32)) + " (" + String(analogReadMilliVolts(32)) + " мВ)");
        b.Label("GPIO 33", String(analogRead(33)) + " (" + String(analogReadMilliVolts(33)) + " мВ)");
        b.Label("GPIO 34", String(analogRead(34)) + " (" + String(analogReadMilliVolts(34)) + " мВ)");
      }
      {
        sets::Row r(b, "GPIO 35 - 39", sets::DivType::Line);
        b.Label("GPIO 35", String(analogRead(35)) + " (" + String(analogReadMilliVolts(35)) + " мВ)");
        b.Label("GPIO 36", String(analogRead(36)) + " (" + String(analogReadMilliVolts(36)) + " мВ)");
        b.Label("GPIO 39", String(analogRead(39)) + " (" + String(analogReadMilliVolts(39)) + " мВ)");
      }
    }
  }
}

// 🖥️ ОБНОВЛЕНИЕ ЭКРАНА LCD1602 (ГЛАВНАЯ СТРАНИЦА: ВВЕРХУ ДАТА И ВРЕМЯ, СНИЗУ ТЕМПЕРАТУРА!)
void updateDisplayWindow() {
  if (otaInProgress) return;

  char line1[65];
  char line2[65];

  struct tm timeinfo;
  bool timeValid = getLocalTime(&timeinfo);
  switch (currentDispMode) {
    case DISP_MAIN: {
      // ВВЕРХУ: ДАТА И ВРЕМЯ (например: 18.08.26  04:05)
      if (timeValid) {
        snprintf(line1, sizeof(line1), "%02d.%02d.%02d  %02d:%02d", 
                 timeinfo.tm_mday, timeinfo.tm_mon + 1, (timeinfo.tm_year + 1900) % 100,
                 timeinfo.tm_hour, timeinfo.tm_min);
      } else {
        uint32_t sec = millis() / 1000;
        uint32_t mins = (sec / 60) % 60;
        uint32_t hrs = (sec / 3600) % 24;
        snprintf(line1, sizeof(line1), "--.--.--  %02d:%02d", hrs, mins);
      }

      // СНИЗУ: ОТОБРАЖЕНИЕ ВЫБРАННОГО ДАТЧИКА (0: AHT10, 1: DS18B20, 2: Оба, 3: Среднее)
      bool hasHum = (climateOk && !isnan(currentHum) && currentHum > 0.0f);
      bool hasAhtTemp = (climateOk && !isnan(currentTemp));
      bool hasDsTemp = (ds18Ok && !isnan(tempDS18));

      char humStr[8] = "--%";
      if (hasHum) snprintf(humStr, sizeof(humStr), "%.0f%%", currentHum);

      switch (cfg.dispTempSource) {
        case 1: { // DS18B20 (Эталон)
          if (hasDsTemp) {
            snprintf(line2, sizeof(line2), "DS:%.1fC  H:%s", tempDS18, humStr);
          } else {
            snprintf(line2, sizeof(line2), "DS:--.-C  H:%s", humStr);
          }
          break;
        }

        case 2: { // Оба датчика сразу (AHT + DS)
          char tA[7] = "--.-";
          char tD[7] = "--.-";
          if (hasAhtTemp) snprintf(tA, sizeof(tA), "%.1f", currentTemp);
          if (hasDsTemp) snprintf(tD, sizeof(tD), "%.1f", tempDS18);
          snprintf(line2, sizeof(line2), "A%s D%s H%s", tA, tD, humStr);
          break;
        }

        case 3: { // Среднее арифметическое (AHT + DS)
          if (hasAhtTemp && hasDsTemp) {
            float avgT = (currentTemp + tempDS18) / 2.0f;
            snprintf(line2, sizeof(line2), "Ср:%.1fC  H:%s", avgT, humStr);
          } else if (hasAhtTemp) {
            snprintf(line2, sizeof(line2), "Ср:%.1fC(A)H:%s", currentTemp, humStr);
          } else if (hasDsTemp) {
            snprintf(line2, sizeof(line2), "Ср:%.1fC(D)H:%s", tempDS18, humStr);
          } else {
            snprintf(line2, sizeof(line2), "Ср:--.-C  H:%s", humStr);
          }
          break;
        }

        default: { // 0: AHT10 (Микроклимат)
          if (hasAhtTemp) {
            snprintf(line2, sizeof(line2), "AHT:%.1fC H:%s", currentTemp, humStr);
          } else {
            snprintf(line2, sizeof(line2), "AHT:--.-C H:%s", humStr);
          }
          break;
        }
      }
      break;
    }

    case DISP_WIFI: {
      if (wifiSleeping) {
        snprintf(line1, sizeof(line1), "Wi-Fi: Сон (Эко)");
        snprintf(line2, sizeof(line2), "Нажмите кнопку  ");
        break;
      }

      if (savedApsCount > 0) {
        selectedSavedApIndex = constrain(selectedSavedApIndex, 0, savedApsCount - 1);
        bool isActive = (strcmp(cfg.wifiSsid, savedAPs[selectedSavedApIndex].ssid) == 0 && WiFi.status() == WL_CONNECTED);
        snprintf(line1, sizeof(line1), "%c%d/%d %s", isActive ? '*' : '>', selectedSavedApIndex + 1, savedApsCount, savedAPs[selectedSavedApIndex].ssid);
      } else {
        if (WiFi.status() == WL_CONNECTED) {
          snprintf(line1, sizeof(line1), "Wi-Fi: Подключен");
        } else {
          snprintf(line1, sizeof(line1), "AP: Incubator");
        }
      }

      if (WiFi.status() == WL_CONNECTED) {
        String ip = WiFi.localIP().toString();
        if (ip.length() <= 12) {
          snprintf(line2, sizeof(line2), "IP: %s", ip.c_str());
        } else {
          snprintf(line2, sizeof(line2), "IP:%s", ip.c_str());
        }
      } else {
        String apIp = WiFi.softAPIP().toString();
        snprintf(line2, sizeof(line2), "AP: %s", apIp.c_str());
      }
      break;
    }

    case DISP_SETTINGS: {
      menu.refresh();
      return;
    }

    case DISP_CALIB_DS: {
      float liveDs = (!isnan(tempDS18)) ? tempDS18 : lastRawTemp;
      if (dsCalibState == 1) {
        snprintf(line1, sizeof(line1), "Кал. DS18 (тело)");
        if (liveDs < 35.0f) {
          snprintf(line2, sizeof(line2), "Грей>35C: %.1fC", liveDs);
        } else if (dsCalibStableCount > 0) {
          snprintf(line2, sizeof(line2), "T:%.1fC  %2d/60с", liveDs, dsCalibStableCount);
        } else {
          snprintf(line2, sizeof(line2), "T:%.1fC  Ждем...", liveDs);
        }
      } else if (dsCalibState == 2) {
        snprintf(line1, sizeof(line1), "DS18 Готово!");
        snprintf(line2, sizeof(line2), "Сдвиг: %+.2fC", calDsOffset);
      } else if (dsCalibState == 3) {
        snprintf(line1, sizeof(line1), "Ошибка калибр.!");
        snprintf(line2, sizeof(line2), "Таймаут >5 мин");
      } else {
        snprintf(line1, sizeof(line1), "Кал. DS18");
        snprintf(line2, sizeof(line2), "Отменено");
      }
      break;
    }

    case DISP_CALIB_AHT: {
      if (ahtCalibState == 1) {
        snprintf(line1, sizeof(line1), "Кал. AHT по DS");
        if (!ds18Ok || isnan(tempDS18)) {
          snprintf(line2, sizeof(line2), "DS18B20 отключен");
        } else if (ahtCalibStableCount > 0) {
          snprintf(line2, sizeof(line2), "D%.1f A%.1f %2dс", tempDS18, currentTemp, ahtCalibStableCount);
        } else {
          snprintf(line2, sizeof(line2), "D:%.1f  A:%.1f", tempDS18, currentTemp);
        }
      } else if (ahtCalibState == 2) {
        snprintf(line1, sizeof(line1), "AHT10 Готово!");
        snprintf(line2, sizeof(line2), "Сдвиг: %+.2fC", cfg.tempOffset);
      } else if (ahtCalibState == 3) {
        snprintf(line1, sizeof(line1), "Ошибка калибр.!");
        snprintf(line2, sizeof(line2), "Датчик не готов");
      } else {
        snprintf(line1, sizeof(line1), "Кал. AHT10");
        snprintf(line2, sizeof(line2), "Отменено");
      }
      break;
    }
  }

  lcd.setCursor(0, 0);
  printCyrillicPadded(line1, 16);
  lcd.setCursor(0, 1);
  printCyrillicPadded(line2, 16);
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[SYSTEM START] Incubator & Sushka Firmware...");

  prefs.begin("incubator", false);
  loadSavedAPs();

  int savedCpuFreq = prefs.getInt("cpuFreq", 1);
  if (savedCpuFreq == 0) setCpuFrequencyMhz(240);
  else if (savedCpuFreq == 1) setCpuFrequencyMhz(160);
  else if (savedCpuFreq == 2) setCpuFrequencyMhz(80);

  cfg.tempOffset = prefs.getFloat("offset", 0.0f);
  cfg.humOffset = prefs.getFloat("humOffset", 0.0f);
  calDsOffset = prefs.getFloat("dsOffset", 0.0f);
  cfg.gmtOffsetHours = prefs.getInt("gmt", 5);
  cfg.dispTempSource = prefs.getInt("tempSrc", 0);

  // Загрузка калибровок клавиатуры из памяти
  cfg.keypadPin = prefs.getInt("keypadPin", 35);
  cfg.adcCenter = prefs.getInt("adcCenter", 248);
  cfg.adcMenu   = prefs.getInt("adcMenu", 372);
  cfg.adcLeft   = prefs.getInt("adcLeft", 682);
  cfg.adcRight  = prefs.getInt("adcRight", 1191);
  cfg.adcBack   = prefs.getInt("adcBack", 1985);
  cfg.adcTol    = prefs.getInt("adcTol", 70);

  String savedSsid = prefs.getString("ssid", "Salomanov");
  String savedPass = prefs.getString("pass", "salomanov");
  snprintf(cfg.wifiSsid, sizeof(cfg.wifiSsid), "%s", savedSsid.c_str());
  snprintf(cfg.wifiPass, sizeof(cfg.wifiPass), "%s", savedPass.c_str());

  analogSetAttenuation(ADC_11db);
  analogReadResolution(12);

  lcd.begin(16, 2);
  delay(50);
  lcd.clear();

  menu.setBackSign("< Назад");
  menu.onPrint([](const char* str, size_t len) {
    if (!str) return;
    for (size_t i = 0; i < len; i++) {
      printCyrillicChar((uint8_t)str[i]);
    }
  });
  menu.onCursor([](uint8_t row, bool chosen, bool active) -> uint8_t {
    lcd.setCursor(0, row);
    lcd.write(chosen ? (active ? 0xFF : '>') : ' ');
    return 1;
  });
  menu.onBuild(buildLcdMenu);

  Wire.begin(21, 22);
  Wire.setClock(100000);
  delay(100);

  initAHT10();
  tickAHT10();

  pinMode(PIN_DS18B20, INPUT_PULLUP);
  delay(50);
  ds18.reset();
  delay(20);
  ds18.setResolution(12);
  ds18.requestTemp();

  // 1. ИНИЦИАЛИЗАЦИЯ WI-FI И СЕТТИНГС
  setupWiFiIncubatorStyle();

  sett.begin();
  sett.onBuild(buildSettingsUI);

  // 2. ИНИЦИАЛИЗАЦИЯ СЛУЖБЫ СЕТЕВОЙ ПРОШИВКИ ПО ВОЗДУХУ (OTA OVER WI-FI)
  setupOTAOverNetwork();

  updateDisplayWindow();
  lastActivityMs = millis();
  tempUpdateTimer.start();
}

void loop() {
  if (!wifiSleeping) {
    sett.tick();
    WiFiConnector.tick();
    ArduinoOTA.handle();
    tickWiFiScan();
    tickWiFiInactivity();
  }
  tickDS18();
  tickAHT10();
  tickBtnCalib();

  // Обновление данных в веб-интерфейсе ТОЛЬКО тогда, когда значения или статусы реально изменились
  static String prevDsStatus = "";
  static String prevAhtStatus = "";
  static String prevHumStatus = "";
  static String prevBtnMsg = "";
  static int prevDsState = 0;
  static int prevAhtState = 0;
  static int prevHumState = 0;
  static int prevBtnStep = 0;

  bool uiNeedsReload = false;

  if (dsCalibStatus != prevDsStatus || dsCalibState != prevDsState) {
    prevDsStatus = dsCalibStatus;
    prevDsState = dsCalibState;
    uiNeedsReload = true;
  }
  if (ahtCalibStatus != prevAhtStatus || ahtCalibState != prevAhtState) {
    prevAhtStatus = ahtCalibStatus;
    prevAhtState = ahtCalibState;
    uiNeedsReload = true;
  }
  if (humTestStatus != prevHumStatus || humTestState != prevHumState) {
    prevHumStatus = humTestStatus;
    prevHumState = humTestState;
    uiNeedsReload = true;
  }
  if (btnCalibMsg != prevBtnMsg || btnCalibStep != prevBtnStep) {
    prevBtnMsg = btnCalibMsg;
    prevBtnStep = btnCalibStep;
    uiNeedsReload = true;
  }

  if (uiNeedsReload && !wifiSleeping) {
    sett.reload();
  }

  // --- УПРАВЛЕНИЕ КНОПКАМИ ---
  Button currentBtn = readButtonsDebounced();

  if (currentBtn != BTN_NONE) {
    wakeUpWiFi(); // Мгновенное пробуждение Wi-Fi при нажатии любой кнопки!

    if (currentBtn == BTN_MENU) {
      if (currentDispMode == DISP_CALIB_DS && dsCalibState == 1) {
        dsCalibState = 0;
        dsCalibStatus = "Отменено пользователем";
      }
      if (currentDispMode == DISP_CALIB_AHT && ahtCalibState == 1) {
        ahtCalibState = 0;
        ahtCalibStatus = "Отменено пользователем";
      }
      if (currentDispMode == DISP_MAIN) currentDispMode = DISP_WIFI;
      else if (currentDispMode == DISP_WIFI) currentDispMode = DISP_SETTINGS;
      else currentDispMode = DISP_MAIN;
      updateDisplayWindow();
    } 
    else if (currentDispMode == DISP_MAIN) {
      if (currentBtn == BTN_RIGHT) {
        currentDispMode = DISP_WIFI;
        updateDisplayWindow();
      } else if (currentBtn == BTN_LEFT) {
        currentDispMode = DISP_SETTINGS;
        updateDisplayWindow();
      } else if (currentBtn == BTN_CENTER) {
        currentDispMode = DISP_WIFI;
        updateDisplayWindow();
      }
    }
    else if (currentDispMode == DISP_WIFI) {
      if (currentBtn == BTN_RIGHT) {
        if (savedApsCount > 0) {
          selectedSavedApIndex = (selectedSavedApIndex + 1) % savedApsCount;
          updateDisplayWindow();
        }
      } else if (currentBtn == BTN_LEFT) {
        if (savedApsCount > 0) {
          if (selectedSavedApIndex > 0) selectedSavedApIndex--;
          else selectedSavedApIndex = savedApsCount - 1;
          updateDisplayWindow();
        }
      } else if (currentBtn == BTN_CENTER) {
        if (savedApsCount > 0) {
          selectedSavedApIndex = constrain(selectedSavedApIndex, 0, savedApsCount - 1);
          snprintf(cfg.wifiSsid, sizeof(cfg.wifiSsid), "%s", savedAPs[selectedSavedApIndex].ssid);
          snprintf(cfg.wifiPass, sizeof(cfg.wifiPass), "%s", savedAPs[selectedSavedApIndex].pass);
          prefs.putString("ssid", cfg.wifiSsid);
          prefs.putString("pass", cfg.wifiPass);
          WiFiConnector.connect(cfg.wifiSsid, cfg.wifiPass);
          updateDisplayWindow();
        }
      } else if (currentBtn == BTN_BACK) {
        currentDispMode = DISP_MAIN;
        updateDisplayWindow();
      }
    }
    else if (currentDispMode == DISP_SETTINGS) {
      if (currentBtn == BTN_CENTER) {
        menu.set();
      } else if (currentBtn == BTN_LEFT) {
        menu.up();
      } else if (currentBtn == BTN_RIGHT) {
        menu.down();
      } else if (currentBtn == BTN_BACK) {
        menu.back();
      }
    }
    else if (currentDispMode == DISP_CALIB_DS) {
      if (currentBtn == BTN_BACK || currentBtn == BTN_CENTER) {
        if (dsCalibState == 1) {
          dsCalibState = 0;
          dsCalibStatus = "Отменено пользователем";
        }
        currentDispMode = DISP_SETTINGS;
        updateDisplayWindow();
      }
    }
    else if (currentDispMode == DISP_CALIB_AHT) {
      if (currentBtn == BTN_BACK || currentBtn == BTN_CENTER) {
        if (ahtCalibState == 1) {
          ahtCalibState = 0;
          ahtCalibStatus = "Отменено пользователем";
        }
        currentDispMode = DISP_SETTINGS;
        updateDisplayWindow();
      }
    }
    else if (currentBtn == BTN_BACK) {
      currentDispMode = DISP_MAIN;
      updateDisplayWindow();
    }
  }

  if (tempUpdateTimer.isReady()) {
    if (currentDispMode != DISP_SETTINGS) {
      updateDisplayWindow();
    }
  }
}
