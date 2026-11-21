$PSScriptRoot = Split-Path -Parent -Path $MyInvocation.MyCommand.Definition
$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")

Write-Host "🎨 コードの自動整形(Format)を開始します..." -ForegroundColor Cyan

# clang-format がインストールされているか確認
$clangFormatPath = Get-Command "clang-format" -ErrorAction SilentlyContinue

if (-not $clangFormatPath) {
    Write-Host "❌ エラー: 'clang-format' が見つかりませんでした。" -ForegroundColor Red
    Write-Host "💡 【解決策】" -ForegroundColor Yellow
    Write-Host "1. LLVM (clang-format) をPCにインストールしてPATHを通すか、" -ForegroundColor Yellow
    Write-Host "2. VS Codeの拡張機能 (C/C++) の標準フォーマッター機能 (Shift+Alt+F) をご利用ください。" -ForegroundColor Yellow
    exit 1
}

# プロジェクト内の .cpp, .h ファイルを一括フォーマットする
# ※ 除外したいディレクトリ（サードパーティライブラリなど）があればここにフィルタを追加してください
$sourceFiles = Get-ChildItem -Path (Join-Path $ProjectRoot "project") -Include *.cpp, *.h -Recurse

$count = 0
foreach ($file in $sourceFiles) {
    # -i オプションでファイルを直接上書きフォーマット
    & $clangFormatPath -i $file.FullName
    $count++
}

Write-Host "✅ $count 個のソースファイルをフォーマットしました！" -ForegroundColor Green
