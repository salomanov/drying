# Project Guardian — Universal Development Guardian for Antigravity

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform: Windows PowerShell](https://img.shields.io/badge/Platform-PowerShell-blue.svg)]()
[![Target: Web | Desktop | Firmware](https://img.shields.io/badge/Target-Web%20%7C%20Desktop%20%7C%20Firmware-green.svg)]()

**Project Guardian** — универсальный скилл и набор глобальных правил для Google Antigravity и Gemini CLI.

Адаптирован под любые задачи:
- 🌐 **Веб-сайты и веб-приложения** (Frontend, Backend, Fullstack, REST API)
- 🖥️ **Программы для ПК** (PyQt6/PySide, C# .NET, Electron, сборка `.exe` и инсталляторов)
- 🔥 **Прошивки микроконтроллеров** (ARM Cortex, STM32, Teensy, ESP32, PlatformIO)

---

## Ключевые возможности

1. 🔄 **Git Auto-Push**: автоматический коммит и `git push` при наличии настроенного `remote`.
2. 🧠 **Universal Project Memory (`PROJECT_STATE.md`)**: сохранение контекста проекта — агент сразу понимает стек, команды запуска и статус при открытии нового диалога.
3. 🛡️ **Safety Checkpoints & Easy Rollback**: бэкапы перед опасными правками и мгновенный откат (`rollback.ps1 -List`).
4. 📦 **Несгораемые архивы релизов без перезаписи**:
   - Для **прошивок**: сохранение бинарников (`.bin`, `.hex`, `.elf`, `.uf2`) в `.backups/firmware_success_YYYY-MM-dd_HH-mm-ss/`.
   - Для **ПК-программ**: сохранение исполняемых файлов (`.exe`, `.msi`, `.dll`) в `.backups/desktop_release_YYYY-MM-dd_HH-mm-ss/` с паспортом SHA256.
   - Для **веб-сайтов**: сохранение продакшн сборки (`dist/`, `build/`, `out/`) в `.backups/web_release_YYYY-MM-dd_HH-mm-ss/`.
5. 🔍 **Автономный подбор скиллов**: подтягивание нужных скиллов из каталога [sickn33/agentic-awesome-skills](https://github.com/sickn33/agentic-awesome-skills) под нужный стек без напоминаний.
6. 📊 **Mandatory Graphify**: построение графа зависимостей кодовой базы для любого проекта.
7. ⚡ **Ponytail Pragmatic Mode**: жесткий режим борьбы с оверинжинирингом — чистый минимальный код, экономия более 50% токенов, отсечение ненужных библиотек.

---

## Структура

```text
/
├── SKILL.md                          # Манифест скилла с триггерами и регламентом
├── scripts/
│   ├── checkpoint.ps1                # Универсальный скрипт бэкапа (код / web / desktop / firmware)
│   ├── rollback.ps1                  # Менеджер отката с просмотром точек (-List)
│   └── install-skill.ps1             # Автономный загрузчик скиллов из GitHub
├── references/
│   └── PROJECT_STATE_TEMPLATE.md     # Универсальный шаблон памяти проекта
├── .gitignore
├── PROJECT_STATE.md                  # Память данного репозитория
└── README.md
```

---

## Использование

### 1. Чекпоинт кода (перед правками)
```powershell
& ".\scripts\checkpoint.ps1" -Message "Рефакторинг роутов"
```

### 2. Релиз веб-сайта
```powershell
& ".\scripts\checkpoint.ps1" -Message "Релиз v1.0.0" -Category "Web" -Release
```

### 3. Релиз программы для ПК
```powershell
& ".\scripts\checkpoint.ps1" -Message "Сборка ПК-версии v1.2" -Category "Desktop" -Release
```

### 4. Удачная прошивка микроконтроллера
```powershell
& ".\scripts\checkpoint.ps1" -Message "Успешный flash v2.1" -Firmware
```

### 5. Просмотр и откат
```powershell
# Список сохраненных точек:
& ".\scripts\rollback.ps1" -List

# Откат к последней точке:
& ".\scripts\rollback.ps1"

# Откат к конкретной точке:
& ".\scripts\rollback.ps1" -TargetCheckpoint "desktop_release_2026-09-28_15-00-00"
```
