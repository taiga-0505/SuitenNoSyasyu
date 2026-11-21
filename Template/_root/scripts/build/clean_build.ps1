param(
    [ValidateSet("Debug", "Development", "Release")]
    [string]$Configuration = "Debug"
)

$PSScriptRoot = Split-Path -Parent -Path $MyInvocation.MyCommand.Definition
$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")

Write-Host "🧹 クリーンビルドを開始します..." -ForegroundColor Cyan

# キャッシュファイルの削除
# pso_cache.bin はゲーム側 Resources/Shader に書き出される（旧配置の Chaso 側も念のため消す）
foreach ($psoCachePath in @(
    (Join-Path $ProjectRoot "project\Resources\Shader\pso_cache.bin"),
    (Join-Path $ProjectRoot "project\Chaso\Resources\Shader\pso_cache.bin"))) {
    if (Test-Path $psoCachePath) {
        Remove-Item -Force $psoCachePath
        Write-Host "🗑️  削除完了: $psoCachePath" -ForegroundColor Yellow
    }
}

$vsFolder = Join-Path $ProjectRoot ".vs"
if (Test-Path $vsFolder) {
    # .vs フォルダは使用中で消せないファイルがある場合があるため、エラーを無視する
    Remove-Item -Recurse -Force $vsFolder -ErrorAction SilentlyContinue
    Write-Host "🗑️  削除完了: .vs キャッシュフォルダ" -ForegroundColor Yellow
}

Write-Host "🔨 通常のビルドスクリプトを呼び出します..." -ForegroundColor Cyan
$buildScript = Join-Path $PSScriptRoot "build_and_run.ps1"
& $buildScript -Configuration $Configuration -Rebuild

Write-Host "✅ クリーンビルドプロセスが完了しました。" -ForegroundColor Green
