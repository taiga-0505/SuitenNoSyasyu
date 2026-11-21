<#
.SYNOPSIS
  ChasoEngine リポジトリの最新をゲームリポジトリの project/Chaso に取り込む。

.DESCRIPTION
  git subtree pull --squash で取り込む。エンジン側の履歴は 1 コミットにまとまる。
  競合した場合は通常の git と同じように解決して commit する。

.EXAMPLE
  .\project\Chaso\scripts\engine-pull.ps1
#>
param(
  [string]$Remote = "https://github.com/taiga-0505/ChasoEngine.git",
  [string]$Branch = "main",
  [string]$Prefix = "project/Chaso"
)
$ErrorActionPreference = "Stop"

$root = git rev-parse --show-toplevel 2>$null
if (-not $root) { throw "git リポジトリの中で実行してください" }
Set-Location $root

$dirty = git status --porcelain
if ($dirty) {
  Write-Host "未コミットの変更があります。subtree pull はマージなので、先にコミットか stash してください。" -ForegroundColor Yellow
  exit 1
}

Write-Host "==> $Remote ($Branch) → $Prefix" -ForegroundColor Cyan
git subtree pull --prefix=$Prefix $Remote $Branch --squash -m "engine: pull ChasoEngine $Branch"
if ($LASTEXITCODE -ne 0) {
  Write-Host "競合が発生しました。解決後に 'git add' → 'git commit' してください。" -ForegroundColor Yellow
  exit $LASTEXITCODE
}
Write-Host "完了: project/Chaso を更新しました" -ForegroundColor Green
