param(
    [string]$ProjectPath = (Get-Location).Path,
    [string]$TargetCheckpoint = "",
    [switch]$List = $false
)

$ErrorActionPreference = "Stop"

Write-Host "=== [Project Guardian] Менеджер восстановления и отката ===" -ForegroundColor Yellow
Write-Host "Проект: $ProjectPath"

$backupRoot = Join-Path $ProjectPath ".backups"

# 1. Поиск контрольных точек и бэкапов всех типов
if (-not (Test-Path $backupRoot)) {
    Write-Host "Папка .backups отсутствует в $ProjectPath. Нет доступных точек восстановления." -ForegroundColor Gray
    return
}

$checkpoints = Get-ChildItem -Path $backupRoot -Directory | Where-Object { 
    $_.Name -like "checkpoint*" -or 
    $_.Name -like "firmware_success_*" -or 
    $_.Name -like "desktop_release_*" -or 
    $_.Name -like "web_release_*"
} | Sort-Object CreationTime -Descending

if ($checkpoints.Count -eq 0) {
    Write-Host "В папке .backups пока нет сохраненных точек восстановления." -ForegroundColor Gray
    return
}

# Режим вывода списка доступных точек
if ($List) {
    Write-Host "`nДоступные точки восстановления (от новых к старым):" -ForegroundColor Cyan
    foreach ($cp in $checkpoints) {
        $icon = "🛡️ [Код]"
        if ($cp.Name -like "firmware_success_*") { $icon = "🔥 [Прошивка]" }
        elseif ($cp.Name -like "desktop_release_*") { $icon = "🖥️ [ПК-Релиз]" }
        elseif ($cp.Name -like "web_release_*") { $icon = "🌐 [Веб-Релиз]" }
        elseif ($cp.Name -like "checkpoint_web_*") { $icon = "🌐 [Веб-Чекпоинт]" }
        elseif ($cp.Name -like "checkpoint_desktop_*") { $icon = "🖥️ [ПК-Чекпоинт]" }

        Write-Host "  $icon $($cp.Name) (создан: $($cp.CreationTime))"
    }
    return
}

$selectedCheckpoint = $null
if ($TargetCheckpoint -ne "") {
    $selectedCheckpoint = $checkpoints | Where-Object { $_.Name -eq $TargetCheckpoint } | Select-Object -First 1
    if (-not $selectedCheckpoint) {
        Write-Warning "Точка отката '$TargetCheckpoint' не найдена среди доступных."
        return
    }
} else {
    $selectedCheckpoint = $checkpoints[0]
}

Write-Host "`nВыбрана точка для восстановления: $($selectedCheckpoint.Name)" -ForegroundColor Cyan
Write-Host "Дата создания: $($selectedCheckpoint.CreationTime)"

# 2. Восстановление исходного кода
$excludeDirs = @(".git", ".backups", "node_modules", ".venv", "venv", "firmware_binaries", "desktop_binaries", "web_artifacts")

try {
    $backupItems = Get-ChildItem -Path $selectedCheckpoint.FullName -Force
    foreach ($item in $backupItems) {
        if ($excludeDirs -contains $item.Name) { continue }
        $destination = Join-Path $ProjectPath $item.Name
        Copy-Item -Path $item.FullName -Destination $destination -Recurse -Force
    }
    Write-Host "[OK] Исходный код проекта успешно восстановлен из: $($selectedCheckpoint.Name)" -ForegroundColor Green

    # Проверка сохраненных артефактов
    $artifactDirs = @("firmware_binaries", "desktop_binaries", "web_artifacts")
    foreach ($ad in $artifactDirs) {
        $fullAd = Join-Path $selectedCheckpoint.FullName $ad
        if (Test-Path $fullAd) {
            Write-Host "  [Артефакты сборки сохранены в]: $fullAd" -ForegroundColor Magenta
        }
    }
}
catch {
    Write-Warning "Ошибка при копировании файлов из бэкапа: $_"
}

# 3. Если Git активен, выводим статус
try {
    $gitCheck = & git -C $ProjectPath rev-parse --is-inside-work-tree 2>$null
    if ($LASTEXITCODE -eq 0 -and $gitCheck.Trim() -eq "true") {
        Write-Host "Статус Git после отката:" -ForegroundColor Cyan
        & git -C $ProjectPath status --short
    }
} catch {
    # Git опционален
}

Write-Host "`nОткат успешно завершен!" -ForegroundColor Green
