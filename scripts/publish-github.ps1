<#
.SYNOPSIS
  このゲームリポジトリを GitHub に公開（初回 push）する。

.DESCRIPTION
  - origin が未設定なら:
      * GitHub CLI (gh) が入っていれば、リポジトリを作成してそのまま push
      * 無ければ、GitHub で作った空リポジトリの URL を聞いて origin に登録 → push
  - origin が設定済みなら、現在のブランチを push するだけ

.EXAMPLE
  .\project\Chaso\scripts\publish-github.ps1
  .\project\Chaso\scripts\publish-github.ps1 -Visibility public
#>
param(
  [ValidateSet("private", "public")]
  [string]$Visibility = "private",
  [string]$Owner = "taiga-0505"
)
$ErrorActionPreference = "Stop"

$root = git rev-parse --show-toplevel 2>$null
if (-not $root) { throw "git リポジトリの中で実行してください" }
Set-Location $root
$name   = Split-Path -Leaf $root
$branch = git branch --show-current

# 未コミットの変更があれば止める
if (git status --porcelain) {
  Write-Host "未コミットの変更があります。先にコミットしてください。" -ForegroundColor Yellow
  git status --short
  exit 1
}

$origin = git remote get-url origin 2>$null
if ($origin) {
  Write-Host "==> origin は設定済み: $origin" -ForegroundColor Cyan
  git push -u origin $branch
  exit $LASTEXITCODE
}

# --- origin 未設定: GitHub にリポジトリを作る ---
$gh = Get-Command gh -ErrorAction SilentlyContinue
if ($gh) {
  Write-Host "==> gh で $Owner/$name を作成して push ($Visibility)" -ForegroundColor Cyan
  gh repo create "$Owner/$name" "--$Visibility" --source=. --remote=origin --push
  if ($LASTEXITCODE -ne 0) {
    Write-Host "gh でのリポジトリ作成に失敗しました。'gh auth login' でログインしてから再実行してください。" -ForegroundColor Yellow
    exit $LASTEXITCODE
  }
} else {
  Write-Host "GitHub CLI (gh) が見つからないので、手動で作ります。" -ForegroundColor Yellow
  Write-Host "  1. https://github.com/new を開く"
  Write-Host "  2. Repository name に '$name' を入れ、README / .gitignore / license は追加せずに作成"
  Write-Host "  3. 表示される URL を貼り付ける"
  Write-Host ""
  Start-Process "https://github.com/new?name=$name"
  $url = Read-Host "リポジトリの URL（例: https://github.com/$Owner/$name.git）"
  if (-not $url) { $url = "https://github.com/$Owner/$name.git" }
  git remote add origin $url
  git push -u origin $branch
  if ($LASTEXITCODE -ne 0) {
    Write-Host "push に失敗しました。GitHub 側にリポジトリができているか、URL が正しいか確認してください。" -ForegroundColor Yellow
    exit $LASTEXITCODE
  }
}

Write-Host ""
Write-Host "完了: $(git remote get-url origin)" -ForegroundColor Green
