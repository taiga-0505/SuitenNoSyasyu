<#
.SYNOPSIS
  ゲームリポジトリ内で行ったエンジン (project/Chaso) の変更を ChasoEngine リポジトリへ送る。

.DESCRIPTION
  git subtree push を使い、project/Chaso 配下に対するコミットだけを切り出して
  ChasoEngine の main ブランチへ push する。ゲーム側のコミットは送られない。
  初回（ChasoEngine が空のリポジトリ）でもそのまま使える。

.EXAMPLE
  .\project\Chaso\scripts\engine-push.ps1
  .\project\Chaso\scripts\engine-push.ps1 -Branch feature/xxx
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

if (-not (Test-Path $Prefix)) { throw "$Prefix が見つかりません（ゲームリポジトリのルートで実行してください）" }

# Resources/ で直したエンジン側リソースを Chaso/Resources に書き戻してから確認する
& (Join-Path $root "$Prefix/scripts/sync-resources.ps1")

$dirty = git status --porcelain -- $Prefix
if ($dirty) {
  Write-Host "project/Chaso に未コミットの変更があります。先にコミットしてください:" -ForegroundColor Yellow
  $dirty | ForEach-Object { Write-Host "  $_" }
  exit 1
}

Write-Host "==> $Prefix → $Remote ($Branch)" -ForegroundColor Cyan
git subtree push --prefix=$Prefix $Remote $Branch
if ($LASTEXITCODE -ne 0) {
  Write-Host ""
  Write-Host "push に失敗しました。別のゲームから先にエンジンが更新されている場合は、" -ForegroundColor Yellow
  Write-Host "先に engine-pull.ps1 で取り込んでから、もう一度 push してください。" -ForegroundColor Yellow
  exit $LASTEXITCODE
}
Write-Host "完了: ChasoEngine を更新しました" -ForegroundColor Green
