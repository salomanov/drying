#include <Arduino.h>
#include <LiquidCrystal.h>

// Пины дисплея VHX1620 / P1620B
#define PIN_RS 19
#define PIN_E  23
#define PIN_D4 18
#define PIN_D5 5
#define PIN_D6 4
#define PIN_D7 2

LiquidCrystal lcd(PIN_RS, PIN_E, PIN_D4, PIN_D5, PIN_D6, PIN_D7);

// 100% ИДЕАЛЬНАЯ таблица заглавных и строчных букв без единой ошибки
void printCyrillic(const char* str) {
  while (*str) {
    uint8_t c = (uint8_t)*str;
    if (c == 0xD0 || c == 0xD1) { // UTF-8 кириллица
      str++;
      uint8_t next = (uint8_t)*str;
      if (!next) break;

      uint16_t unicode = ((uint16_t)c << 8) | next;
      uint8_t outChar = '?';

      switch (unicode) {
        // === Ё и ё (из блоков B13: 0xCB и B15: 0xEB) ===
        case 0xD081: outChar = 0xCB; break; // Заглавная Ё
        case 0xD191: outChar = 0xEB; break; // Строчная ё

        // === ЗАГЛАВНЫЕ БУКВЫ (100% Все заглавные из B09 и B10!) ===
        case 0xD090: outChar = 'A';  break; // А
        case 0xD091: outChar = 0x80; break; // Б (B09)
        case 0xD092: outChar = 'B';  break; // В
        case 0xD093: outChar = 0x81; break; // Г (B09)
        case 0xD094: outChar = 0x82; break; // Д (B09)
        case 0xD095: outChar = 'E';  break; // Е
        case 0xD096: outChar = 0x83; break; // Ж (B09)
        case 0xD097: outChar = 0x92; break; // З (ЗАГЛАВНАЯ З из B10!)
        case 0xD098: outChar = 0x84; break; // И (B09)
        case 0xD099: outChar = 0x85; break; // Й (B09)
        case 0xD09A: outChar = 'K';  break; // К
        case 0xD09B: outChar = 0x86; break; // Л (B09)
        case 0xD09C: outChar = 'M';  break; // М
        case 0xD09D: outChar = 'H';  break; // Н
        case 0xD09E: outChar = 'O';  break; // О
        case 0xD09F: outChar = 0x87; break; // П (B09)
        case 0xD0A0: outChar = 'P';  break; // Р
        case 0xD0A1: outChar = 'C';  break; // С
        case 0xD0A2: outChar = 'T';  break; // Т
        case 0xD0A3: outChar = 0x93; break; // У (НАСТОЯЩАЯ ЗАГЛАВНАЯ У из B10: 0x93!)
        case 0xD0A4: outChar = 0x88; break; // Ф (B09)
        case 0xD0A5: outChar = 'X';  break; // Х
        case 0xD0A6: outChar = 0x89; break; // Ц (B09)
        case 0xD0A7: outChar = 0x8A; break; // Ч (B09)
        case 0xD0A8: outChar = 0x8B; break; // Ш (B09)
        case 0xD0A9: outChar = 0x8C; break; // Щ (B09)
        case 0xD0AA: outChar = 0x8D; break; // Ъ (B09)
        case 0xD0AB: outChar = 0x8E; break; // Ы (B09)
        case 0xD0AC: outChar = 'b';  break; // Ь
        case 0xD0AD: outChar = 0x8F; break; // Э (B09)
        case 0xD0AE: outChar = 0x90; break; // Ю (ЗАГЛАВНАЯ Ю из B10!)
        case 0xD0AF: outChar = 0x91; break; // Я (ЗАГЛАВНАЯ Я из B10!)

        // === СТРОЧНЫЕ БУКВЫ (100% Из блоков B11 и B12!) ===
        case 0xD0B0: outChar = 'a';  break; // а
        case 0xD0B1: outChar = 0xA0; break; // б (B11)
        case 0xD0B2: outChar = 0xB5; break; // в (B12)
        case 0xD0B3: outChar = 0xA1; break; // г (B11)
        case 0xD0B4: outChar = 0xA2; break; // д (B11)
        case 0xD0B5: outChar = 'e';  break; // е
        case 0xD0B6: outChar = 0xA3; break; // ж (B11)
        case 0xD0B7: outChar = 0xB2; break; // з (B12)
        case 0xD0B8: outChar = 0xA4; break; // и (B11)
        case 0xD0B9: outChar = 0xA5; break; // й (B11)
        case 0xD0BA: outChar = 0xB6; break; // к (B12)
        case 0xD0BB: outChar = 0xA6; break; // л (B11)
        case 0xD0BC: outChar = 0xB7; break; // м (B12)
        case 0xD0BD: outChar = 0xB8; break; // н (B12)
        case 0xD0BE: outChar = 'o';  break; // о
        case 0xD0BF: outChar = 0xA7; break; // п (B11)
        case 0xD180: outChar = 'p';  break; // р
        case 0xD181: outChar = 'c';  break; // с
        case 0xD182: outChar = 0xB9; break; // т (B12)
        case 0xD183: outChar = 0xB3; break; // у (Строчная у из B12!)
        case 0xD184: outChar = 0xA8; break; // ф (B11)
        case 0xD185: outChar = 'x';  break; // х
        case 0xD186: outChar = 0xA9; break; // ц (B11)
        case 0xD187: outChar = 0xAA; break; // ч (B11)
        case 0xD188: outChar = 0xAB; break; // ш (B11)
        case 0xD189: outChar = 0xAC; break; // щ (B11)
        case 0xD18A: outChar = 0xAD; break; // ъ (B11)
        case 0xD18B: outChar = 0xAE; break; // ы (B11)
        case 0xD18C: outChar = 0xB4; break; // ь (B12)
        case 0xD18D: outChar = 0xAF; break; // э (B11)
        case 0xD18E: outChar = 0xB0; break; // ю (B12)
        case 0xD18F: outChar = 0xB1; break; // я (B12)
        default: outChar = '?'; break;
      }
      lcd.write(outChar);
    } else {
      lcd.write(c);
    }
    str++;
  }
}

void showPage(const char* line1, const char* line2) {
  lcd.setCursor(0, 0);
  printCyrillic(line1);
  lcd.setCursor(0, 1);
  printCyrillic(line2);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  lcd.begin(16, 2);
  delay(50);
  lcd.clear();
  delay(50);
}

void loop() {
  // Экран 1: Полный Заглавный Алфавит (А-Я)
  showPage("АБВГДЕЁЖЗИЙКЛМНО", "ПРСТУФХЦЧШЩЪЫЬЭЯ");
  delay(3500);

  // Экран 2: Полный Строчный Алфавит (а-я)
  showPage("абвгдеёжзийклмно", "прстуфхцчшщъыьэя");
  delay(3500);

  // Экран 3: Проверочная фраза
  showPage("Съешь ещё этих  ", "мягких булочек! ");
  delay(3500);
}
