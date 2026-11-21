<#
.SYNOPSIS
  このゲームリポジトリを GitHub に公開（初回 push）する。

.DESCRIPTION
  -Mode school   : 学校用。公開先は -Owner（既定 taiga-0505）
  -Mode external : 学校以外（外部）用。公開先はリポジトリ直下の .chaso.json の githubOwner
                   （無ければ聞く）。origin の URL に「アカウント名@」を付けて登録するので、
                   Git Credential Manager がこのリポジトリだけ別アカウントで認証する。

  - origin が未設定なら:
      * GitHub CLI (gh) が入っていれば、リポジトリを作成して push
        （external では gh のアカウントを一時的に切り替え、終わったら元に戻す）
      * 無ければ、GitHub で作った空リポジトリの URL を聞いて origin に登録 → push
  - origin が設定済みなら、現在のブランチを push するだけ

.EXAMPLE
  .\project\Chaso\scripts\publish-github.ps1
  .\project\Chaso\scripts\publish-github.ps1 -Mode external
  .\project\Chaso\scripts\publish-github.ps1 -Visibility public
#>
param(
  [ValidateSet("school", "external")]
  [string]$Mode = "school",
  [ValidateSet("private", "public")]
  [string]$Visibility = "private",
  [string]$Owner
)
$ErrorActionPreference = "Stop"

# stderr を出しうる外部コマンドを、例外にせず静かに実行する（Windows PowerShell 5.1 対策）
function Invoke-Quiet([scriptblock]$Block) {
  $old = $ErrorActionPreference
  $ErrorActionPreference = "Continue"
  try { & $Block 2>$null } finally { $ErrorActionPreference = $old }
}

$root = git rev-parse --show-toplevel 2>$null
if (-not $root) { throw "git リポジトリの中で実行してください" }
Set-Location $root
$name   = Split-Path -Leaf $root
$branch = git branch --show-current

# リポジトリの種類（.chaso.json があり mode = external なら外部ゲーム）
$chasoCfg = $null
$chasoCfgPath = Join-Path $root ".chaso.json"
if (Test-Path $chasoCfgPath) {
  try { $chasoCfg = Get-Content $chasoCfgPath -Raw -Encoding UTF8 | ConvertFrom-Json } catch {}
}
$repoIsExternal = ($chasoCfg -and $chasoCfg.mode -eq "external")

if ($Mode -eq "school" -and $repoIsExternal) {
  Write-Host "このリポジトリは外部用（.chaso.json の mode = external）です。学校アカウントには公開しません。" -ForegroundColor Yellow
  Write-Host "「GitHub公開(別アカウント)」を使ってください。" -ForegroundColor Yellow
  exit 1
}

if (-not $Owner) {
  if ($Mode -eq "school") {
    $Owner = "taiga-0505"
  } elseif ($chasoCfg -and $chasoCfg.githubOwner) {
    $Owner = $chasoCfg.githubOwner
  } else {
    $Owner = (Read-Host "公開先の GitHub アカウント名").Trim()
    if (-not $Owner) { throw "GitHub アカウント名が空です" }
  }
}

# 未コミットの変更があれば止める
if (git status --porcelain) {
  Write-Host "未コミットの変更があります。先にコミットしてください。" -ForegroundColor Yellow
  git status --short
  exit 1
}

$origin = Invoke-Quiet { git remote get-url origin }
if ($origin) {
  Write-Host "==> origin は設定済み: $origin" -ForegroundColor Cyan
  git push -u origin $branch
  exit $LASTEXITCODE
}

# external: URL にアカウント名を入れて、このリポジトリだけ別アカウントで認証させる
if ($Mode -eq "external") {
  $repoUrl = "https://$Owner@github.com/$Owner/$name.git"
} else {
  $repoUrl = "https://github.com/$Owner/$name.git"
}

# --- origin 未設定: GitHub にリポジトリを作る ---
$gh = Get-Command gh -ErrorAction SilentlyContinue
if ($gh) {
  Write-Host "==> gh で $Owner/$name を作成 ($Visibility)" -ForegroundColor Cyan

  # external: gh のアクティブアカウントを $Owner に切り替える（終わったら戻す）
  $restoreUser = $null
  if ($Mode -eq "external") {
    $current = Invoke-Quiet { gh api user --jq .login }
    if ($current -ne $Owner) {
      Invoke-Quiet { gh auth switch --hostname github.com --user $Owner } | Out-Null
      if ($LASTEXITCODE -ne 0) {
        Write-Host "gh に $Owner でログインしていません。次を実行してから、もう一度ボタンを押してください:" -ForegroundColor Yellow
        Write-Host "  gh auth login   （ブラウザで $Owner としてログイン。既存のアカウントはそのまま残ります）"
        exit 1
      }
      if ($current) { $restoreUser = $current }
    }
  }

  try {
    gh repo create "$Owner/$name" "--$Visibility" --source=. --remote=origin
    $createExit = $LASTEXITCODE
  } finally {
    if ($restoreUser) { Invoke-Quiet { gh auth switch --hostname github.com --user $restoreUser } | Out-Null }
  }
  if ($createExit -ne 0) {
    Write-Host "gh でのリポジトリ作成に失敗しました。'gh auth login' でログインしてから再実行してください。" -ForegroundColor Yellow
    exit $createExit
  }
  git remote set-url origin $repoUrl
} else {
  Write-Host "GitHub CLI (gh) が見つからないので、手動で作ります。" -ForegroundColor Yellow
  Write-Host "  1. https://github.com/new を開く（ブラウザが $Owner でログインしているか確認）"
  Write-Host "  2. Repository name に '$name' を入れ、README / .gitignore / license は追加せずに作成"
  Write-Host "  3. 表示される URL を貼り付ける（Enter のみ: $repoUrl）"
  Write-Host ""
  Start-Process "https://github.com/new?name=$name"
  $url = (Read-Host "リポジトリの URL").Trim()
  if (-not $url) { $url = $repoUrl }
  if ($Mode -eq "external" -and $url -match '^https://github\.com/') {
    $url = $url -replace '^https://github\.com/', "https://$Owner@github.com/"
  }
  git remote add origin $url
}

Write-Host "==> push: $(git remote get-url origin)" -ForegroundColor Cyan
if ($Mode -eq "external") {
  Write-Host "  初回はブラウザで $Owner としてサインインを求められることがあります。" -ForegroundColor DarkGray
}
git push -u origin $branch
if ($LASTEXITCODE -ne 0) {
  Write-Host "push に失敗しました。GitHub 側にリポジトリができているか、URL・ログイン中のアカウントが正しいか確認してください。" -ForegroundColor Yellow
  exit $LASTEXITCODE
}

Write-Host ""
Write-Host "完了: $(git remote get-url origin)" -ForegroundColor Green
