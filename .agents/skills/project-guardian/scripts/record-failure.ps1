<#
.SYNOPSIS
    Автоматическая немедленная фиксация сбоев, аппаратных граблей и инцидентов.
.DESCRIPTION
    Записывает ошибку в docs/ERRORS_AND_FAILURES.md, запускает graphify для обновления
    графа знаний, создает защитную контрольную точку в .backups и пушит в Git.
.PARAMETER Title
    Краткое название сбоя/ошибки.
.PARAMETER Symptom
    Симптом и ущерб (что наблюдалось, поведение системы).
.PARAMETER RootCause
    Аппаратная или программная первопричина (Root Cause).
.PARAMETER Solution
    Решение, правило предотвращения или способ восстановления.
.PARAMETER ProjectPath
    Путь к проекту (по умолчанию текущая директория).
.PARAMETER NoCheckpoint
    Не запускать автоматический checkpoint после записи.
#>

param(
    [Parameter(Mandatory=$true)]
    [string]$Title,

    [Parameter(Mandatory=$false)]
    [string]$Symptom = "Не указан",

    [Parameter(Mandatory=$false)]
    [string]$RootCause = "Не указана",

    [Parameter(Mandatory=$false)]
    [string]$Solution = "Не указано",

    [Parameter(Mandatory=$false)]
    [string]$ProjectPath = $PWD.Path,

    [Parameter(Mandatory=$false)]
    [switch]$NoCheckpoint
)

$ErrorActionPreference = "Stop"

Write-Host "`n=== [Project Guardian] ⚠️ Немедленная фиксация инцидента/ошибки ===" -ForegroundColor Yellow
Write-Host "Проект: $ProjectPath"
Write-Host "Ошибка: $Title"

# 1. Поиск или создание файла базы ошибок
$docsDir = Join-Path $ProjectPath "docs"
$errorFile = Join-Path $docsDir "ERRORS_AND_FAILURES.md"

if (-not (Test-Path $errorFile)) {
    # Проверяем в корне
    $rootErrorFile = Join-Path $ProjectPath "ERRORS_AND_FAILURES.md"
    if (Test-Path $rootErrorFile) {
        $errorFile = $rootErrorFile
    } else {
        if (-not (Test-Path $docsDir)) {
            New-Item -ItemType Directory -Path $docsDir -Force | Out-Null
        }
        $header = @"
# База знаний ошибок, граблей и неудачных экспериментов (FAILURE LOG)
> **Назначение файла**: Полный реестр всех ошибочных решений, тупиковых гипотез и проверенных антипаттернов.
> **Правило разработчика**: ПЕРЕД любым изменением пинов, регистров или архитектуры ОБЯЗАТЕЛЬНО свериться с этим файлом!

---

"@
        [System.IO.File]::WriteAllText($errorFile, $header, [System.Text.Encoding]::UTF8)
        Write-Host "[OK] Создан новый файл базы ошибок: $errorFile" -ForegroundColor Green
    }
}

# 2. Формирование записи ошибки
$timestamp = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
$entry = @"

---

### [Инцидент $timestamp] $Title
- **Симптом и ущерб**: $Symptom
- **Причина (Root Cause)**: $RootCause
- **Решение и правило**: $Solution

"@

[System.IO.File]::AppendAllText($errorFile, $entry, [System.Text.Encoding]::UTF8)
Write-Host "[OK] Инцидент зафиксирован в $errorFile" -ForegroundColor Green

# 3. Автоматический запуск чекпоинта и актуализация graphify
if (-not $NoCheckpoint) {
    $checkpointScript = Join-Path $PSScriptRoot "checkpoint.ps1"
    if (Test-Path $checkpointScript) {
        Write-Host "`n[Project Guardian] Запуск авто-чекпоинта и актуализации graphify..." -ForegroundColor Cyan
        & $checkpointScript -ProjectPath $ProjectPath -Message "failure: $Title"
    } else {
        Write-Warning "Скрипт checkpoint.ps1 не найден: $checkpointScript"
    }
}
