<#
.SYNOPSIS
    ChasoEngine - CLI Build & Run Script
.DESCRIPTION
    MSBuild to build the solution, and launch ChasoApp.exe only on success.
    Working directory is set to project/ (same as VS debugger).
.PARAMETER Configuration
    Build configuration (Debug, Development, Release). Default: Debug
.PARAMETER BuildOnly
    Build only, skip exe launch.
.PARAMETER RunOnly
    Skip build, launch existing exe.
.PARAMETER Rebuild
    Full rebuild instead of incremental build.
#>
param(
    [ValidateSet("Debug", "Development", "Release")]
    [string]$Configuration = "Debug",

    [switch]$BuildOnly,
    [switch]$RunOnly,
    [switch]$Rebuild
)

# ============================================================
# Path definitions
# ============================================================
$ScriptDir     = $PSScriptRoot
$ProjectRoot   = (Resolve-Path (Join-Path $ScriptDir "..\..")).Path

$SolutionFile  = Join-Path $ProjectRoot "project\chaso.sln"
$OutputDir     = Join-Path $ProjectRoot "generated\outputs\$Configuration"
$ExePath       = Join-Path $OutputDir "ChasoApp.exe"
$WorkingDir    = Join-Path $ProjectRoot "project"
$LogDir        = Join-Path $ProjectRoot "logs\build"
$Timestamp     = Get-Date -Format "yyyy-MM-dd_HH-mm-ss"
$LogFile       = Join-Path $LogDir "build_${Configuration}_${Timestamp}.log"

# ============================================================
# Find MSBuild via vswhere
# ============================================================
function Find-MSBuild {
    $progX86 = [System.Environment]::GetFolderPath("ProgramFilesX86")
    if (-not $progX86) { $progX86 = "C:\Program Files (x86)" }

    $vswherePath = Join-Path $progX86 "Microsoft Visual Studio\Installer\vswhere.exe"

    if (Test-Path $vswherePath) {
        $vsPath = & $vswherePath -latest -property installationPath 2>$null
        if ($vsPath) {
            $msbuild = Join-Path $vsPath "MSBuild\Current\Bin\amd64\MSBuild.exe"
            if (Test-Path $msbuild) { return $msbuild }
            $msbuild = Join-Path $vsPath "MSBuild\Current\Bin\MSBuild.exe"
            if (Test-Path $msbuild) { return $msbuild }
        }
    }

    $msbuildCmd = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
    if ($msbuildCmd) { return $msbuildCmd.Source }

    return $null
}

# ============================================================
# Output helpers
# ============================================================
function Write-Header($msg) {
    Write-Host ""
    Write-Host "============================================================" -ForegroundColor Cyan
    Write-Host "  $msg" -ForegroundColor Cyan
    Write-Host "============================================================" -ForegroundColor Cyan
    Write-Host ""
}

function Write-OK($msg)    { Write-Host "[OK] $msg" -ForegroundColor Green }
function Write-Err($msg)   { Write-Host "[ERROR] $msg" -ForegroundColor Red }
function Write-Info($msg)  { Write-Host "[INFO] $msg" -ForegroundColor Yellow }

# ============================================================
# Main
# ============================================================
Clear-Host
Write-Header "ChasoEngine - Build & Run ($Configuration)"

# Verify solution file
if (-not (Test-Path $SolutionFile)) {
    Write-Err "Solution file not found: $SolutionFile"
    exit 1
}

# Create log directory
if (-not (Test-Path $LogDir)) {
    New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
}

# ----------------------------------------------------------
# Build Phase
# ----------------------------------------------------------
if (-not $RunOnly) {
    $MSBuild = Find-MSBuild
    if (-not $MSBuild) {
        Write-Err "MSBuild not found. Please ensure Visual Studio 2022 is installed."
        exit 1
    }

    Write-Info "MSBuild: $MSBuild"
    Write-Info "Solution: $SolutionFile"
    Write-Info "Configuration: $Configuration | Platform: x64"
    Write-Info "Log: $LogFile"
    Write-Host ""

    $BuildTarget = "Build"
    if ($Rebuild) { $BuildTarget = "Rebuild" }

    $buildArgs = @(
        "`"$SolutionFile`""
        "/t:$BuildTarget"
        "/p:Configuration=$Configuration"
        "/p:Platform=x64"
        "/m"
        "/nologo"
        "/v:minimal"
        "/fl"
        "/flp:logfile=`"$LogFile`";Encoding=UTF-8;verbosity=normal"
    )

    Write-Header "Build starting..."
    $buildStart = Get-Date

    & $MSBuild @buildArgs
    $buildExitCode = $LASTEXITCODE

    $buildEnd = Get-Date
    $elapsed = $buildEnd - $buildStart
    $buildDuration = "{0:mm\:ss\.fff}" -f $elapsed

    Write-Host ""
    if ($buildExitCode -eq 0) {
        Write-OK "Build succeeded! (Duration: $buildDuration)"
        Write-Info "Full log: $LogFile"
    } else {
        Write-Err "Build FAILED (Exit code: $buildExitCode, Duration: $buildDuration)"
        Write-Err "Check the log: $LogFile"
        Write-Host ""
        Write-Host "--- Last errors ---" -ForegroundColor Red
        if (Test-Path $LogFile) {
            Get-Content $LogFile -Encoding UTF8 |
                Select-String "error " |
                Select-Object -Last 10 |
                ForEach-Object { Write-Host $_.Line -ForegroundColor Red }
        }
        exit $buildExitCode
    }

    if ($BuildOnly) {
        Write-Info "BuildOnly mode: skipping launch."
        exit 0
    }
}

# ----------------------------------------------------------
# Run Phase
# ----------------------------------------------------------
Write-Header "Launching ChasoApp"

if (-not (Test-Path $ExePath)) {
    Write-Err "Executable not found: $ExePath"
    Write-Err "Please build first."
    exit 1
}

Write-Info "Exe: $ExePath"
Write-Info "Working Directory: $WorkingDir"
Write-Host ""

# Launch with working directory = project/ (matches VS debugger behavior)
# 起動に失敗したら（OS のアプリ制御でブロックされた等）、ここで止めて原因を表示する
try {
    $process = Start-Process -FilePath $ExePath `
                             -WorkingDirectory $WorkingDir `
                             -NoNewWindow -PassThru -ErrorAction Stop
} catch {
    $launchError = $_.Exception.Message
    Write-Err "Failed to launch ChasoApp: $launchError"

    # 4551 = ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION（スマート アプリ コントロール / App Control for Business）
    # Start-Process は Win32Exception を包まずに文言だけ渡すので、OS の言語に合わせた文言で照合する
    $policyMessage = (New-Object System.ComponentModel.Win32Exception 4551).Message.Trim()
    if ($policyMessage -and $launchError.Contains($policyMessage)) {
        Write-Host ""
        Write-Info "Windows blocked ChasoApp.exe by an application control policy (usually Smart App Control)."
        Write-Info "Check: Windows Security > App & browser control > Smart App Control settings"
        Write-Info "Details: Event Viewer > Applications and Services Logs > Microsoft > Windows > CodeIntegrity > Operational (Event ID 3077 / 3033)"
    }
    exit 1
}

# ExitCode を後で読めるよう、終了前にハンドルを取得しておく（-Wait なしの Start-Process の既知の挙動）
$null = $process.Handle

Write-OK "ChasoApp launched (PID: $($process.Id))"
Write-Info "Process will run inline. Press Ctrl+C to force exit if stuck."

# Wait for the process to exit
$process.WaitForExit()
$appExitCode = $process.ExitCode
if ($null -eq $appExitCode) { $appExitCode = 1 }

Write-Host ""
if ($appExitCode -eq 0) {
    Write-OK "ChasoApp exited normally."
} else {
    Write-Err "ChasoApp exited with error (Exit code: $appExitCode)"
}

exit $appExitCode
