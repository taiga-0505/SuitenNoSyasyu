<#
.SYNOPSIS
  ChasoEngine を使った新しいゲームリポジトリを作る。

.DESCRIPTION
  1. 空の git リポジトリを作成
  2. ChasoEngine を project/Chaso に subtree add（--squash）
  3. project/Chaso/Template の中身（main.cpp, sln, vcxproj, AppConfig.json など）を project/ にコピー
  4. 初回コミット

  出来上がった構成:
    <Dest>/<Name>/
      .gitignore, .project-actions.json, .vscode/, scripts/build/   ← Template/_root から
      project/
        chaso.sln, main.cpp
        Application/ChasoApp.vcxproj, Application/Game/Scripts/
        Resources/Scenes/Title.json      ← ゲームのリソース
        Resources/Setting/               ← AppConfig.json, GameSettings.json
        Chaso/                           ← エンジン（subtree）

.EXAMPLE
  .\project\Chaso\scripts\new-game.ps1 -Name MyNewGame -Dest D:\
  .\project\Chaso\scripts\new-game.ps1            # 名前と作成先を聞かれる

  -Dest を省略した場合は作成先をターミナルで聞く（Enter で初期値、? でフォルダ選択ダイアログ）。
  選んだ場所は %APPDATA%\Chaso\new-game.json に PC ごとに記憶し、次回の初期値になる。
#>
param(
  [string]$Name,
  [string]$Dest,
  [string]$Remote = "https://github.com/taiga-0505/ChasoEngine.git",
  [string]$Branch = "main"
)
$ErrorActionPreference = "Stop"

if (-not $Name) {
  $Name = Read-Host "新しいゲームの名前（リポジトリ名）"
  if (-not $Name) { throw "名前が空です" }
}

if (-not $Dest) {
  $configDir  = Join-Path $env:APPDATA "Chaso"
  $configFile = Join-Path $configDir "new-game.json"

  # 初期値: 前回の作成先 → D:\production → ユーザーフォルダ
  $lastDest = $null
  if (Test-Path $configFile) {
    try { $lastDest = (Get-Content $configFile -Raw -Encoding UTF8 | ConvertFrom-Json).LastDest } catch {}
  }
  $initial = @($lastDest, "D:\production", $env:USERPROFILE) |
    Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1

  # ターミナルで聞く（Enter = 初期値、? = フォルダ選択ダイアログ）
  Write-Host "作成先の親フォルダ（<親フォルダ>\$Name が作られます）"
  Write-Host "  Enter のみ: $initial / ? : フォルダ選択ダイアログを開く" -ForegroundColor DarkGray
  $answer = (Read-Host "作成先").Trim().Trim('"')

  if ($answer -eq "?") {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description         = "「$Name」を作成する親フォルダを選んでください"
    $dialog.SelectedPath        = $initial
    $dialog.ShowNewFolderButton = $true

    # エディタの裏に隠れないよう、画面外に最前面の小さなフォームを実際に表示してオーナーにする
    $owner = New-Object System.Windows.Forms.Form -Property @{
      TopMost = $true; ShowInTaskbar = $false; StartPosition = "Manual"
      Location = New-Object System.Drawing.Point(-2000, -2000); Size = New-Object System.Drawing.Size(1, 1)
    }
    try {
      $owner.Show(); $owner.Activate()
      Write-Host "ダイアログを開きました（見当たらなければ Alt+Tab で探してください）" -ForegroundColor DarkGray
      $result = $dialog.ShowDialog($owner)
    } finally {
      $owner.Close(); $owner.Dispose()
    }
    if ($result -ne [System.Windows.Forms.DialogResult]::OK -or -not $dialog.SelectedPath) {
      Write-Host "キャンセルされました。" -ForegroundColor Yellow
      exit 1
    }
    $Dest = $dialog.SelectedPath
  } elseif ($answer) {
    $Dest = [Environment]::ExpandEnvironmentVariables($answer)
  } else {
    $Dest = $initial
  }

  # 次回の初期値として記憶
  if (-not (Test-Path $configDir)) { New-Item -ItemType Directory -Path $configDir | Out-Null }
  @{ LastDest = $Dest } | ConvertTo-Json | Set-Content $configFile -Encoding UTF8
}
if (-not (Test-Path $Dest)) { New-Item -ItemType Directory -Path $Dest | Out-Null }
Write-Host "作成先: $(Join-Path $Dest $Name)" -ForegroundColor Cyan

$target = Join-Path $Dest $Name
if (Test-Path $target) { throw "$target は既に存在します" }

New-Item -ItemType Directory -Path $target | Out-Null
Set-Location $target

git init -b master | Out-Null
# subtree add には 1 つ以上のコミットが必要なので、先に README だけコミットする
"# $Name`n`nChasoEngine ベースのゲーム。エンジン本体は project/Chaso（ChasoEngine の subtree）。`n" |
  Set-Content -Encoding UTF8 README.md
git add README.md
git commit -q -m "init: $Name"

Write-Host "==> ChasoEngine を project/Chaso に取り込み" -ForegroundColor Cyan
git subtree add --prefix=project/Chaso $Remote $Branch --squash -m "engine: add ChasoEngine $Branch"

Write-Host "==> テンプレートを展開" -ForegroundColor Cyan
$tpl = Join-Path $target "project\Chaso\Template"
# _root/ 以外 → project/ 、 _root/ の中身 → リポジトリ直下
Get-ChildItem -Path $tpl -Force | Where-Object { $_.Name -ne "_root" } | ForEach-Object {
  Copy-Item -Path $_.FullName -Destination (Join-Path $target "project") -Recurse -Force
}
$tplRoot = Join-Path $tpl "_root"
if (Test-Path $tplRoot) {
  Get-ChildItem -Path $tplRoot -Force | ForEach-Object {
    Copy-Item -Path $_.FullName -Destination $target -Recurse -Force
  }
}
# タイトルをゲーム名に（構成別）
$cfg = Join-Path $target "project\Resources\Setting\AppConfig.json"
if (Test-Path $cfg) {
  $j = Get-Content $cfg -Raw -Encoding UTF8 | ConvertFrom-Json
  $j.title = [ordered]@{ Debug = "$Name (Debug)"; Development = "$Name (Development)"; Release = $Name }
  $j | ConvertTo-Json -Depth 5 | Set-Content $cfg -Encoding UTF8
}

# エンジンのリソースを project/Resources に展開（ビルド前にも自動で走る）
& (Join-Path $target "project\Chaso\scripts\sync-resources.ps1")

git add -A
git commit -q -m "game: bootstrap from ChasoEngine template"

Write-Host ""
Write-Host "完了: $target" -ForegroundColor Green
Write-Host "  Visual Studio で project\chaso.sln を開いてビルドしてください。"
Write-Host "  エンジンを直したら: .\project\Chaso\scripts\engine-push.ps1"
Write-Host "  エンジンの更新を取り込む: .\project\Chaso\scripts\engine-pull.ps1"
