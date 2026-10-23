param(
    [string]$Message = "Automatic checkpoint",
    [switch]$Push = $true,
    [string]$Category = "Auto", # "Auto", "Code", "Web", "Desktop", "Firmware"
    [switch]$Firmware = $false,  # Обратная совместимость
    [switch]$Release = $false,   # Флаг сохранения релизного билда (exe, web dist, бинарник)
    [switch]$NoGraphify = $false,# Пропустить автоматическое обновление графа graphify
    [string]$ProjectPath = (Get-Location).Path
)

$ErrorActionPreference = "Stop"

# 1. Определение категории проекта (Автоопределение или явное указание)
if ($Firmware) {
    $Category = "Firmware"
    $Release = $true
}

if ($Category -eq "Auto") {
    # Поиск маркеров микроконтроллеров/прошивок
    $isFirmwareProject = (Test-Path (Join-Path $ProjectPath "platformio.ini")) -or 
                         (Get-ChildItem -Path $ProjectPath -Filter "*.ino" -ErrorAction SilentlyContinue) -or
                         (Get-ChildItem -Path $ProjectPath -Filter "*.ioc" -ErrorAction SilentlyContinue)

    # Поиск маркеров десктопных приложений для ПК
    $isDesktopProject = (Get-ChildItem -Path $ProjectPath -Filter "*.spec" -ErrorAction SilentlyContinue) -or
                        (Get-ChildItem -Path $ProjectPath -Filter "*.csproj" -ErrorAction SilentlyContinue) -or
                        (Get-ChildItem -Path $ProjectPath -Filter "*app.py" -ErrorAction SilentlyContinue) -or
                        (Get-ChildItem -Path $ProjectPath -Filter "*main.py" -ErrorAction SilentlyContinue)

    # Поиск маркеров веб-сайтов и веб-приложений
    $isWebProject = (Test-Path (Join-Path $ProjectPath "package.json")) -or 
                    (Test-Path (Join-Path $ProjectPath "index.html")) -or
                    (Test-Path (Join-Path $ProjectPath "vite.config.*")) -or
                    (Test-Path (Join-Path $ProjectPath "next.config.*"))

    if ($isFirmwareProject) {
        $Category = "Firmware"
    } elseif ($isWebProject) {
        $Category = "Web"
    } elseif ($isDesktopProject) {
        $Category = "Desktop"
    } else {
        $Category = "Code"
    }
}

# 2. Формирование меток и папок
$now = Get-Date
$readableDate = $now.ToString("yyyy-MM-dd HH:mm:ss")
$timestamp = $now.ToString("yyyy-MM-dd_HH-mm-ss")

$backupRoot = Join-Path $ProjectPath ".backups"

$backupPrefix = "checkpoint"
$categoryEmoji = "🛡️"
$categoryTitle = "Чекпоинт Кода"

switch ($Category) {
    "Firmware" {
        $backupPrefix = "firmware_success"
        $categoryEmoji = "🔥"
        $categoryTitle = "УДАЧНАЯ ПРОШИВКА (Firmware Flash)"
        $Release = $true
    }
    "Desktop" {
        if ($Release) {
            $backupPrefix = "desktop_release"
            $categoryEmoji = "🖥️"
            $categoryTitle = "РЕЛИЗ ДЕСКТОП-ПРИЛОЖЕНИЯ (ПК / Desktop Build)"
        } else {
            $backupPrefix = "checkpoint_desktop"
            $categoryEmoji = "🖥️"
            $categoryTitle = "Чекпоинт десктоп-проекта"
        }
    }
    "Web" {
        if ($Release) {
            $backupPrefix = "web_release"
            $categoryEmoji = "🌐"
            $categoryTitle = "РЕЛИЗ ВЕБ-САЙТА (Web Production Build)"
        } else {
            $backupPrefix = "checkpoint_web"
            $categoryEmoji = "🌐"
            $categoryTitle = "Чекпоинт веб-проекта"
        }
    }
    default {
        $backupPrefix = "checkpoint"
        $categoryEmoji = "🛡️"
        $categoryTitle = "Универсальный чекпоинт кода"
    }
}

$currentBackupDir = Join-Path $backupRoot "${backupPrefix}_$timestamp"
if (Test-Path $currentBackupDir) {
    $timestamp = $now.ToString("yyyy-MM-dd_HH-mm-ss_fff")
    $currentBackupDir = Join-Path $backupRoot "${backupPrefix}_$timestamp"
}

Write-Host "=== [Project Guardian] Резервная копия: $categoryEmoji $categoryTitle ===" -ForegroundColor Cyan
Write-Host "Проект: $ProjectPath"
Write-Host "Категория: $Category (Релизный архив: $Release)"
Write-Host "Сообщение: $Message"

# 3. Создание бэкапа
try {
    if (-not (Test-Path $backupRoot)) {
        New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    }

    # Обеспечиваем наличие .backups в .gitignore
    $gitignorePath = Join-Path $ProjectPath ".gitignore"
    $needGitignoreUpdate = $true
    if (Test-Path $gitignorePath) {
        $giContent = Get-Content $gitignorePath -Raw -ErrorAction SilentlyContinue
        if ($giContent -match "^\.backups/" -or $giContent -match "\r?\n\.backups/") {
            $needGitignoreUpdate = $false
        }
    }
    if ($needGitignoreUpdate) {
        Add-Content -Path $gitignorePath -Value "`n.backups/" -Encoding UTF8
        Write-Host "Добавлена запись .backups/ в .gitignore" -ForegroundColor Green
    }

    # Создаем изолированную директорию (никогда не перезаписывает старые)
    New-Item -ItemType Directory -Path $currentBackupDir -Force | Out-Null

    # Исключения зависимостей и временных файлов
    $excludeDirs = @(".git", ".backups", "node_modules", ".venv", "venv", "__pycache__", "dist", "build", ".next", ".nuxt", ".turbo", "bin", "obj")

    # Копирование исходников
    $items = Get-ChildItem -Path $ProjectPath -Force | Where-Object { $excludeDirs -notcontains $_.Name }
    foreach ($item in $items) {
        Copy-Item -Path $item.FullName -Destination $currentBackupDir -Recurse -Force
    }

    # 4. Сохранение релизных артефактов в зависимости от категории
    $savedArtifacts = @()

    if ($Category -eq "Firmware") {
        $firmwareBinDir = Join-Path $currentBackupDir "firmware_binaries"
        New-Item -ItemType Directory -Path $firmwareBinDir -Force | Out-Null
        $binExtensions = @("*.bin", "*.hex", "*.elf", "*.uf2", "*.dfu", "*.img", "*.ota")
        $foundBinaries = Get-ChildItem -Path $ProjectPath -Recurse -Include $binExtensions -File -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notlike "*\.backups\*" }

        foreach ($bin in $foundBinaries) {
            Copy-Item -Path $bin.FullName -Destination (Join-Path $firmwareBinDir $bin.Name) -Force
            $hash = (Get-FileHash -Path $bin.FullName -Algorithm SHA256).Hash
            $savedArtifacts += [PSCustomObject]@{ Name = $bin.Name; SizeKB = [math]::Round($bin.Length / 1KB, 2); SHA256 = $hash }
            Write-Host "  [Бинарник сохранен]: $($bin.Name) ($([math]::Round($bin.Length / 1KB, 2)) KB)" -ForegroundColor Magenta
        }
    }
    elseif ($Category -eq "Desktop" -and $Release) {
        $desktopBinDir = Join-Path $currentBackupDir "desktop_binaries"
        New-Item -ItemType Directory -Path $desktopBinDir -Force | Out-Null
        $exeExtensions = @("*.exe", "*.msi", "*.dll", "*.zip")
        # Ищем в dist/ и корне
        $foundExes = Get-ChildItem -Path $ProjectPath -Recurse -Include $exeExtensions -File -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notlike "*\.backups\*" -and $_.FullName -notlike "*\node_modules\*" -and $_.FullName -notlike "*\.venv\*" }

        foreach ($f in $foundExes) {
            Copy-Item -Path $f.FullName -Destination (Join-Path $desktopBinDir $f.Name) -Force
            $hash = (Get-FileHash -Path $f.FullName -Algorithm SHA256).Hash
            $savedArtifacts += [PSCustomObject]@{ Name = $f.Name; SizeKB = [math]::Round($f.Length / 1KB, 2); SHA256 = $hash }
            Write-Host "  [Десктоп бинарник/EXE сохранен]: $($f.Name) ($([math]::Round($f.Length / 1KB, 2)) KB)" -ForegroundColor Magenta
        }
    }
    elseif ($Category -eq "Web" -and $Release) {
        $webDistDir = Join-Path $currentBackupDir "web_artifacts"
        New-Item -ItemType Directory -Path $webDistDir -Force | Out-Null
        $distFolders = @("dist", "build", "out")
        foreach ($df in $distFolders) {
            $srcDist = Join-Path $ProjectPath $df
            if (Test-Path $srcDist) {
                Copy-Item -Path $srcDist -Destination (Join-Path $webDistDir $df) -Recurse -Force
                Write-Host "  [Веб-сборка сохранена]: $df" -ForegroundColor Magenta
            }
        }
    }

    # Генерация паспорта релиза
    if ($Release -or $Category -eq "Firmware") {
        $passportContent = @"
# Паспорт релиза: $backupPrefix [$timestamp]

- **Категория**: $Category
- **Дата и время**: $readableDate
- **Описание**: $Message
- **Папка архива**: $currentBackupDir

## Сохраненные артефакты:
"@
        if ($savedArtifacts.Count -gt 0) {
            foreach ($a in $savedArtifacts) {
                $passportContent += "`n- **$($a.Name)** ($($a.SizeKB) KB) | SHA256: ``$($a.SHA256)``"
            }
        } else {
            $passportContent += "`n*(Полный снимок рабочего исходного кода сохранен без перезаписи)*"
        }

        $passportFile = Join-Path $currentBackupDir "RELEASE_INFO.md"
        [System.IO.File]::WriteAllText($passportFile, $passportContent, [System.Text.Encoding]::UTF8)
        Write-Host "[OK] Паспорт релиза создан: RELEASE_INFO.md" -ForegroundColor Green
    }

    Write-Host "[OK] Резервная копия сохранена в: $currentBackupDir" -ForegroundColor Green
}
catch {
    Write-Warning "Не удалось создать файловый бэкап в .backups: $_"
}

# 4.1. Автоматическое обновление графа знаний (graphify)
$graphifyDir = Join-Path $ProjectPath "graphify-out"
if ((Test-Path $graphifyDir) -and (-not $NoGraphify)) {
    Write-Host "`n=== Обновление Графа Знаний (Graphify) ===" -ForegroundColor Cyan
    try {
        $pyCmd = Get-Command python -ErrorAction SilentlyContinue
        if ($pyCmd) {
            Write-Host "Инкрементальный анализ кодовой базы через graphify..." -ForegroundColor Gray
            & $pyCmd.Source -m graphify extract $ProjectPath --code-only 2>$null
            & $pyCmd.Source -m graphify cluster-only $ProjectPath 2>$null
            if ($LASTEXITCODE -eq 0) {
                Write-Host "[OK] Граф знаний graphify успешно актуализирован!" -ForegroundColor Green
            } else {
                Write-Host "[!] Завершено с кодом $LASTEXITCODE" -ForegroundColor Yellow
            }
        }
    } catch {
        Write-Warning "Не удалось обновить graphify: $_"
    }
}

# 5. Обработка Git
$isGit = $false
try {
    $gitCheck = & git -C $ProjectPath rev-parse --is-inside-work-tree 2>$null
    if ($LASTEXITCODE -eq 0 -and $gitCheck.Trim() -eq "true") {
        $isGit = $true
    }
} catch {
    $isGit = $false
}

if ($isGit) {
    Write-Host "`n=== Обработка Git ===" -ForegroundColor Cyan
    try {
        & git -C $ProjectPath add -A
        $status = & git -C $ProjectPath status --porcelain
        $tagPrefix = $Category.ToLower()

        $commitPrefix = switch ($Category) {
            "Firmware" { "firmware: [SUCCESS FLASH]" }
            "Desktop"  { if ($Release) { "desktop: [RELEASE BUILD]" } else { "desktop: [checkpoint]" } }
            "Web"      { if ($Release) { "web: [PRODUCTION BUILD]" } else { "web: [checkpoint]" } }
            default    { "checkpoint:" }
        }
        $fullCommitMsg = "$commitPrefix $Message [$timestamp]"

        if ($status) {
            & git -C $ProjectPath commit -m "$fullCommitMsg"
            Write-Host "[OK] Изменения зафиксированы в коммите: $fullCommitMsg" -ForegroundColor Green
        } else {
            Write-Host "В Git нет незафиксированных изменений рабочего дерева." -ForegroundColor Yellow
        }

        # Если это релиз или прошивка — создаем уникальный Git-тег с датой и временем
        if ($Release -or $Category -eq "Firmware") {
            $tagName = "$tagPrefix-v$timestamp"
            & git -C $ProjectPath tag -a $tagName -m "Релиз ($Category): $Message ($readableDate)"
            Write-Host "[OK] Создан уникальный Git-тег: $tagName" -ForegroundColor Magenta
        }

        # Проверка remote для push
        $remotes = & git -C $ProjectPath remote
        if ($remotes -and $Push) {
            $currentBranch = (& git -C $ProjectPath rev-parse --abbrev-ref HEAD).Trim()
            if ($currentBranch -and $currentBranch -ne "HEAD") {
                Write-Host "Отправка изменений в origin/$currentBranch..." -ForegroundColor Cyan
                & git -C $ProjectPath push origin $currentBranch
                if ($Release -or $Category -eq "Firmware") {
                    & git -C $ProjectPath push origin --tags
                }
                if ($LASTEXITCODE -eq 0) {
                    Write-Host "[OK] Успешно отправлено в Git remote (origin/$currentBranch)" -ForegroundColor Green
                } else {
                    Write-Warning "Не удалось выполнить git push. Проверьте права доступа и состояние ветки."
                }
            }
        } elseif (-not $remotes) {
            Write-Host "Git remote не настроен. Коммит сохранен локально." -ForegroundColor Gray
        }
    }
    catch {
        Write-Warning "Ошибка при выполнении операций Git: $_"
    }
} else {
    Write-Host "Папка не является Git-репозиторием. Локальный бэкап успешно создан." -ForegroundColor Gray
}

# 6. Обновление даты в PROJECT_STATE.md
$stateFile = Join-Path $ProjectPath "PROJECT_STATE.md"
if (Test-Path $stateFile) {
    try {
        $stateContent = Get-Content $stateFile -Raw -Encoding UTF8
        $entryPrefix = switch ($Category) {
            "Firmware" { "🔥 **УДАЧНАЯ ПРОШИВКА**" }
            "Desktop"  { if ($Release) { "🖥️ **РЕЛИЗ ДЕСКТОП-ПРИЛОЖЕНИЯ**" } else { "🖥️ **Чекпоинт (ПК)**" } }
            "Web"      { if ($Release) { "🌐 **РЕЛИЗ ВЕБ-САЙТА**" } else { "🌐 **Чекпоинт (Web)**" } }
            default    { "🛡️ **Чекпоинт**" }
        }
        $logEntry = "`n- $entryPrefix [$readableDate] (метка: $timestamp): $Message"
        if ($stateContent -match "(## 5\. История Изменений[^\r\n]*)") {
            $stateContent = $stateContent -replace "(## 5\. История Изменений[^\r\n]*)", "`$1`n$logEntry"
            [System.IO.File]::WriteAllText($stateFile, $stateContent, [System.Text.Encoding]::UTF8)
            Write-Host "[OK] Запись добавлена в PROJECT_STATE.md" -ForegroundColor Green
        }
    } catch {
        Write-Warning "Не удалось обновить PROJECT_STATE.md: $_"
    }
}

Write-Host "`nРезервное копирование завершено успешно! Архив сохранен без перезаписи." -ForegroundColor Green
