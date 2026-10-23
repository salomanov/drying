param(
    [Parameter(Mandatory=$true)]
    [string]$SkillName,
    [string]$DestDir = "C:\Users\Salomanov\.gemini\config\skills"
)

$ErrorActionPreference = "Stop"

Write-Host "=== [Project Guardian] Установка скилла '$SkillName' ===" -ForegroundColor Cyan
Write-Host "Источник: github.com/sickn33/agentic-awesome-skills"

$targetSkillDir = Join-Path $DestDir $SkillName

if (-not (Test-Path $DestDir)) {
    New-Item -ItemType Directory -Path $DestDir -Force | Out-Null
}

function Download-GitHubDirectory {
    param (
        [string]$RepoPath,
        [string]$LocalPath
    )

    if (-not (Test-Path $LocalPath)) {
        New-Item -ItemType Directory -Path $LocalPath -Force | Out-Null
    }

    $apiUrl = "https://api.github.com/repos/sickn33/agentic-awesome-skills/contents/$RepoPath"
    $headers = @{ "User-Agent" = "ProjectGuardian-SkillInstaller" }

    try {
        $response = Invoke-RestMethod -Uri $apiUrl -Headers $headers -TimeoutSec 15
        foreach ($item in $response) {
            $destFile = Join-Path $LocalPath $item.name
            if ($item.type -eq "file") {
                Write-Host "  Скачивание: $($item.path)..." -ForegroundColor Gray
                Invoke-WebRequest -Uri $item.download_url -OutFile $destFile -UseBasicParsing -TimeoutSec 20 | Out-Null
            } elseif ($item.type -eq "dir") {
                Download-GitHubDirectory -RepoPath $item.path -LocalPath $destFile
            }
        }
        return $true
    } catch {
        return $false
    }
}

$installed = $false

# 1. Попытка рекурсивной загрузки через GitHub API
Write-Host "Запрос структуры каталога скилла..." -ForegroundColor Cyan
$apiSuccess = Download-GitHubDirectory -RepoPath "skills/$SkillName" -LocalPath $targetSkillDir

if ($apiSuccess -and (Test-Path (Join-Path $targetSkillDir "SKILL.md"))) {
    $installed = $true
} else {
    # 2. Фолбэк: скачивание SKILL.md напрямую через raw.githubusercontent.com
    Write-Host "Фолбэк: скачивание напрямую через raw.githubusercontent.com..." -ForegroundColor Yellow
    try {
        if (-not (Test-Path $targetSkillDir)) {
            New-Item -ItemType Directory -Path $targetSkillDir -Force | Out-Null
        }
        $rawUrl = "https://raw.githubusercontent.com/sickn33/agentic-awesome-skills/main/skills/$SkillName/SKILL.md"
        $targetSkillFile = Join-Path $targetSkillDir "SKILL.md"
        Invoke-WebRequest -Uri $rawUrl -OutFile $targetSkillFile -UseBasicParsing -TimeoutSec 20 | Out-Null
        if (Test-Path $targetSkillFile) {
            $installed = $true
        }
    } catch {
        Write-Warning "Не удалось загрузить скилл '$SkillName': $_"
    }
}

if ($installed) {
    Write-Host "[OK] Скилл '$SkillName' успешно установлен в: $targetSkillDir" -ForegroundColor Green

    # Обновление .antigravity-install-manifest.json
    $manifestPath = Join-Path $DestDir ".antigravity-install-manifest.json"
    if (Test-Path $manifestPath) {
        try {
            $manifest = Get-Content $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
            if ($manifest.entries -notcontains $SkillName) {
                $manifest.entries += $SkillName
                $manifest.updatedAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ss.fffZ")
                $manifestJson = $manifest | ConvertTo-Json -Depth 5
                [System.IO.File]::WriteAllText($manifestPath, $manifestJson, [System.Text.Encoding]::UTF8)
                Write-Host "[OK] Запись добавлена в манифест скиллов" -ForegroundColor Green
            }
        } catch {
            Write-Warning "Не удалось обновить файл манифеста: $_"
        }
    }
} else {
    Write-Error "Не удалось установить скилл '$SkillName'. Проверьте точное название в каталоге sickn33/agentic-awesome-skills."
}
