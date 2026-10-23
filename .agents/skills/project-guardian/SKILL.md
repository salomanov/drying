---
name: project-guardian
description: >-
  Универсальный хранитель проекта для любых сфер разработки: Веб-сайты (Web), Программы для ПК (Desktop),
  Прошивки микроконтроллеров (Firmware) и Backend. Автоматическое сохранение памяти проекта (PROJECT_STATE.md),
  создание контрольных точек и несгораемых релизных бэкапов без перезаписи, безопасный откат (rollback),
  коммит и пуш в Git при наличии remote, а также автономный подбор скиллов из каталога sickn33/agentic-awesome-skills.
---

# Project Guardian (Универсальный Хранитель Проекта)

Скилл полностью адаптирован под три ключевых направления:
1. 🌐 **Веб-сайты и веб-приложения** (React, Vue, Next.js, HTML/CSS/JS, FastAPI, Flask, Django).
2. 🖥️ **Программы для ПК** (PyQt6/PySide, C# .NET, Electron, Tauri, PyInstaller `.exe`).
3. 🔥 **Прошивки микроконтроллеров** (ARM Cortex, STM32, Teensy, ESP32, Arduino, PlatformIO).

---

## 1. Долговременная память проекта (`PROJECT_STATE.md`)

В корне любого проекта (веб, десктоп или прошивка) ведется файл `PROJECT_STATE.md`:
- **При старте диалога**: агент считывает его и моментально подхватывает контекст (цель, стек, текущее состояние, команды запуска/сборки, открытые задачи).
- **По завершении этапа**: агент актуализирует статус и фиксирует журнал изменений.
- Эталонная структура: [`./references/PROJECT_STATE_TEMPLATE.md`](./references/PROJECT_STATE_TEMPLATE.md).

---

## 2. Контрольные точки и бэкапы (`checkpoint.ps1`)

Скрипт автоматически распознает тип проекта или принимает категорию явно:
```powershell
# Автоматический чекпоинт кода (перед правками):
& "C:\Users\Salomanov\.gemini\config\skills\project-guardian\scripts\checkpoint.ps1" -Message "Рефакторинг логики"

# Релиз веб-сайта (сохраняет продакшн сборку из dist/build):
& "C:\Users\Salomanov\.gemini\config\skills\project-guardian\scripts\checkpoint.ps1" -Message "Релиз сайта v1.0" -Category "Web" -Release

# Релиз программы для ПК (сохраняет .exe, .msi, библиотеки с SHA256):
& "C:\Users\Salomanov\.gemini\config\skills\project-guardian\scripts\checkpoint.ps1" -Message "Сборка ПК утилиты v1.0" -Category "Desktop" -Release

# Удачная прошивка микроконтроллера (сохраняет .bin, .hex, .elf с SHA256):
& "C:\Users\Salomanov\.gemini\config\skills\project-guardian\scripts\checkpoint.ps1" -Message "Успешный flash v1.2" -Firmware
```

### Принцип «Несгораемых архивов»:
- Каждая сборка/прошивка/релиз сохраняется в **отдельную папку с уникальной датой и временем**:
  - `.backups/firmware_success_YYYY-MM-dd_HH-mm-ss/`
  - `.backups/desktop_release_YYYY-MM-dd_HH-mm-ss/`
  - `.backups/web_release_YYYY-MM-dd_HH-mm-ss/`
- Старые релизы **НИКОГДА не перезаписываются**.
- Создаются паспорта сборки `RELEASE_INFO.md` / `FIRMWARE_INFO.md` с контрольными суммами SHA256 и Git-теги.

---

## 3. Немедленная фиксация сбоев и ошибок (`record-failure.ps1`)

При любом непредвиденном поведении, зависании ядра, срыве шины (I2C/SPI), ошибках компиляции или аппаратных граблях — агент **ОБЯЗАН немедленно зафиксировать инцидент ДО повторных попыток**:
```powershell
& "C:\Users\Salomanov\.gemini\config\skills\project-guardian\scripts\record-failure.ps1" `
    -Title "Краткое название сбоя" `
    -Symptom "Что сломалось или зависло" `
    -RootCause "Почему это произошло (железо/код)" `
    -Solution "Как исправлено и железное правило"
```
**Что делает скрипт автоматически:**
1. Записывает инцидент в `docs/ERRORS_AND_FAILURES.md`.
2. Автоматически запускает инкрементальное обновление графа знаний `graphify` (`graphify-out/`).
3. Создает защитный бэкап в `.backups/`.
4. Коммитит и пушит в Git `origin`.

---

## 4. Откат и просмотр истории (`rollback.ps1`)

- **Просмотреть все сохраненные точки**:
  ```powershell
  & "C:\Users\Salomanov\.gemini\config\skills\project-guardian\scripts\rollback.ps1" -List
  ```
- **Откатиться к последнему рабочему состоянию**:
  ```powershell
  & "C:\Users\Salomanov\.gemini\config\skills\project-guardian\scripts\rollback.ps1"
  ```
- **Откатиться к конкретному релизу или прошивке**:
  ```powershell
  & "C:\Users\Salomanov\.gemini\config\skills\project-guardian\scripts\rollback.ps1" -TargetCheckpoint "desktop_release_2026-09-28_15-00-00"
  ```

---

## 5. Автономный подбор специализированных скиллов

В зависимости от того, что мы разрабатываем, агент сам подтягивает специализированные навыки из каталога [sickn33/agentic-awesome-skills](https://github.com/sickn33/agentic-awesome-skills):
- **Для сайтов**: `react-ui-patterns`, `tailwind-patterns`, `fastapi-expert`, `seo-optimization`, `web-performance`.
- **Для ПК программ**: `pyqt6-desktop-dev`, `pyinstaller-windows-packager`, `csharp-dotnet`.
- **Для прошивок**: `arm-cortex-expert`, `firmware-analyst`, `hardware-security`.

Команда установки:
```powershell
& "C:\Users\Salomanov\.gemini\config\skills\project-guardian\scripts\install-skill.ps1" -SkillName "<имя-скилла>"
```

---

## 6. Архитектурный анализ через `graphify`

Для любого проекта (веб-приложение, исходники десктопной программы на Python/C#, репозиторий прошивки на C/C++) агент использует `graphify` для построения графа зависимостей и взаимосвязей файлов в папке `graphify-out/`.
- **Автоматическая актуализация**: скрипт `checkpoint.ps1` при наличии папки `graphify-out/` автоматически запускает инкрементальное обновление графа перед коммитом (`graphify extract --code-only` и `graphify cluster-only`). Для пропуска можно передать флаг `-NoGraphify`.

---

## 7. Прагматичный режим Ponytail (Чистый код и экономия токенов)

Агент во всех задачах руководствуется принципом **«ленивого сеньора» (Ponytail)**:
- **Наименьшее полное изменение побеждает**: лучший код — тот, который не пришлось писать. Решать задачу минимальным объемом нового кода.
- **Никакого оверинжиниринга (YAGNI)**: не создавать фабрики, лишние слои абстракций, обертки или функции «на будущее».
- **Максимум стандартной библиотеки**: не ставить сторонние пакеты (`npm`/`pip`), если задача решается стандартными средствами языка или платформы.
- **Переиспользование**: всегда проверять и использовать уже существующие в проекте утилиты и сервисы.
- **Лаконичность ответов**: четкие ответы по существу, экономия контекста диалога.
