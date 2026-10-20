<#
.SYNOPSIS
    ChasoEngine - 配布パッケージ作成スクリプト
.DESCRIPTION
    指定構成（既定: Release）でビルドし、実行に必要なものだけを集めて zip にまとめる。
    出力先: generated/package/<タイトル>_<日時>/ と同名の .zip（generated/ は .gitignore 済み）

    パッケージの構成（exe は「../project/」「Resources/」を作業ディレクトリ基準で読むため project/ を再現する）:
        <パッケージ名>.zip       … zip 直下が project/（パッケージ名のフォルダは挟まない）
            project/
                ChasoApp.exe, dxcompiler.dll
                Resources/        … .blend などの制作用データは除外
                    Setting/      … AppConfig.json, GameSettings.json

    含めないもの: pdb / map / lib、Resources/Setting/EditorConfig.json、imgui.ini、StageProgress.json（開発中のセーブ）、
                  pso_cache.bin（GPU 依存のキャッシュ）、Resources/blender、*.blend、*.py
.PARAMETER Configuration
    ビルド構成（Debug, Development, Release）。既定: Release
.PARAMETER SkipBuild
    ビルドせず、既存の exe をそのまま詰める。
.PARAMETER NoZip
    フォルダだけ作り、zip にしない。
#>
param(
    [ValidateSet("Debug", "Development", "Release")]
    [string]$Configuration = "Release",

    [switch]$SkipBuild,
    [switch]$NoZip
)

$ErrorActionPreference = "Stop"

# ============================================================
# Path definitions
# ============================================================
$ScriptDir   = $PSScriptRoot
$RepoRoot    = (Resolve-Path (Join-Path $ScriptDir "..\..")).Path
$ProjectDir  = Join-Path $RepoRoot "project"
$OutputDir   = Join-Path $RepoRoot "generated\outputs\$Configuration"
$PackageRoot = Join-Path $RepoRoot "generated\package"

# 実行に必要なファイル
$BinFiles    = @("ChasoApp.exe", "dxcompiler.dll")

# Resources から除外するもの
$ExcludeDirs  = @("blender")
$ExcludeFiles = @("*.blend", "*.blend1", "*.py", "pso_cache.bin", "EditorConfig.json", ".gitignore", "desktop.ini", "Thumbs.db")

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
function Write-OK($msg)   { Write-Host "[OK] $msg" -ForegroundColor Green }
function Write-Err($msg)  { Write-Host "[ERROR] $msg" -ForegroundColor Red }
function Write-Info($msg) { Write-Host "[INFO] $msg" -ForegroundColor Yellow }

function Get-SafeName([string]$name) {
    $invalid = [System.IO.Path]::GetInvalidFileNameChars()
    $sb = New-Object System.Text.StringBuilder
    foreach ($c in $name.ToCharArray()) {
        if ($invalid -contains $c) { [void]$sb.Append('_') } else { [void]$sb.Append($c) }
    }
    return $sb.ToString().Trim()
}

# ============================================================
# 1. Build
# ============================================================
if (-not $SkipBuild) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $ScriptDir "build_and_run.ps1") -Configuration $Configuration -BuildOnly
    if ($LASTEXITCODE -ne 0) {
        Write-Err "ビルドに失敗したのでパッケージ作成を中止します。"
        exit $LASTEXITCODE
    }
}

Write-Header "ChasoEngine - Package ($Configuration)"

foreach ($f in $BinFiles) {
    if (-not (Test-Path (Join-Path $OutputDir $f))) {
        Write-Err "見つかりません: $OutputDir\$f （先にビルドしてください）"
        exit 1
    }
}

# ============================================================
# 2. パッケージ名（AppConfig.json の title.<構成> を使う）
# ============================================================
$title = "ChasoApp"
try {
    $cfg = Get-Content (Join-Path $ProjectDir "Resources\Setting\AppConfig.json") -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($cfg.title -and $cfg.title.$Configuration) { $title = [string]$cfg.title.$Configuration }
} catch {
    Write-Info "AppConfig.json の title を読めなかったので既定名を使います。"
}
$stamp       = Get-Date -Format "yyyyMMdd_HHmm"
$packageName = (Get-SafeName $title) + "_" + $stamp
if ($Configuration -ne "Release") { $packageName += "_$Configuration" }

$StageDir   = Join-Path $PackageRoot $packageName
$StageProj  = Join-Path $StageDir "project"
$ZipPath    = "$StageDir.zip"

if (Test-Path $StageDir) { Remove-Item $StageDir -Recurse -Force }
New-Item -ItemType Directory -Path $StageProj -Force | Out-Null

Write-Info "Package: $StageDir"

# ============================================================
# 3. Copy
# ============================================================
foreach ($f in $BinFiles) {
    Copy-Item (Join-Path $OutputDir $f) $StageProj
}
Write-OK "exe / dll をコピーしました（設定ファイルは Resources/Setting/ として一緒にコピーされる）"

$rcArgs = @(
    (Join-Path $ProjectDir "Resources"), (Join-Path $StageProj "Resources"),
    "/E", "/XD") + $ExcludeDirs + @("/XF") + $ExcludeFiles + @(
    "/NJH", "/NJS", "/NDL", "/NFL", "/NP", "/R:1", "/W:1")
& robocopy @rcArgs | Out-Null
if ($LASTEXITCODE -ge 8) {
    Write-Err "Resources のコピーに失敗しました (robocopy: $LASTEXITCODE)"
    exit 1
}
Write-OK "Resources をコピーしました（除外: $($ExcludeDirs -join ', '), $($ExcludeFiles -join ', ')）"

$sizeMB = [math]::Round(((Get-ChildItem $StageDir -Recurse -File | Measure-Object Length -Sum).Sum / 1MB), 1)
Write-Info "フォルダサイズ: $sizeMB MB"

# ============================================================
# 4. Zip
# ============================================================
if (-not $NoZip) {
    Write-Info "zip を作成中...（サイズによっては少し時間がかかります）"
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
    # 日本語ファイル名が化けないよう UTF-8 でエントリ名を書く
    # includeBaseDirectory=$false: zip 直下を project/ にする（パッケージ名のフォルダを挟まない）
    [System.IO.Compression.ZipFile]::CreateFromDirectory(
        $StageDir, $ZipPath,
        [System.IO.Compression.CompressionLevel]::Optimal,
        $false,
        [System.Text.Encoding]::UTF8)
    $zipMB = [math]::Round((Get-Item $ZipPath).Length / 1MB, 1)
    Write-OK "zip を作成しました: $ZipPath ($zipMB MB)"
    Start-Process explorer.exe "/select,`"$ZipPath`""
} else {
    Start-Process explorer.exe "`"$StageDir`""
}

Write-Host ""
Write-OK "パッケージ作成が完了しました。"
Write-Info "配布先の PC には「Visual C++ 再頒布可能パッケージ (x64)」が必要です。"
exit 0
