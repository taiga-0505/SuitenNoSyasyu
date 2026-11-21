<#
.SYNOPSIS
  ChasoEngine を使った新しいゲームリポジトリを作る。

.DESCRIPTION
  1. 空の git リポジトリを作成
  2. ChasoEngine を project/Chaso に subtree add（--squash）
  3. project/Chaso/Template の中身（main.cpp, sln, vcxproj, AppConfig.json など）を project/ にコピー
  4. 初回コミット

  -External を付けると「外部（学校以外）用」のゲームを作る:
    - Template/_root_external/ の中身で上書き（ボタン構成が外部用になる）
    - リポジトリ直下に .chaso.json（mode = external, githubOwner = 公開先アカウント）を作成
    - コミット用のメールアドレスを指定した場合、このリポジトリだけ user.name / user.email を設定
    公開先アカウントとメールは %APPDATA%\Chaso\new-game.json に PC ごとに記憶し、次回の初期値になる。

  出来上がった構成:
    <Dest>/<Name>/
      .gitignore, .project-actions.json, .vscode/, scripts/build/   ← Template/_root から
      .chaso.json                                                   ← 外部用のみ
      project/
        chaso.sln, main.cpp
        Application/ChasoApp.vcxproj, Application/Game/Scripts/
        Resources/Scenes/Title.json      ← ゲームのリソース
        Resources/Setting/               ← AppConfig.json, GameSettings.json
        Chaso/                           ← エンジン（subtree）

.EXAMPLE
  .\project\Chaso\scripts\new-game.ps1 -Name MyNewGame -Dest D:\
  .\project\Chaso\scripts\new-game.ps1            # 名前と作成先を聞かれる
  .\project\Chaso\scripts\new-game.ps1 -External  # 外部用（公開先アカウントも聞かれる）

  -Dest を省略した場合は作成先をターミナルで聞く（Enter で初期値、? でフォルダ選択ダイアログ）。
  選んだ場所は %APPDATA%\Chaso\new-game.json に PC ごとに記憶し、次回の初期値になる。
#>
param(
  [string]$Name,
  [string]$Dest,
  [switch]$External,
  [string]$Owner,
  [string]$Email,
  [string]$Remote = "https://github.com/taiga-0505/ChasoEngine.git",
  [string]$Branch = "main"
)
$ErrorActionPreference = "Stop"

# PC ごとの記憶（%APPDATA%\Chaso\new-game.json）
$configDir  = Join-Path $env:APPDATA "Chaso"
$configFile = Join-Path $configDir "new-game.json"
$memory = @{}
if (Test-Path $configFile) {
  try {
    $obj = Get-Content $configFile -Raw -Encoding UTF8 | ConvertFrom-Json
    $obj.PSObject.Properties | ForEach-Object { $memory[$_.Name] = $_.Value }
  } catch {}
}
function Save-Memory {
  if (-not (Test-Path $configDir)) { New-Item -ItemType Directory -Path $configDir | Out-Null }
  $memory | ConvertTo-Json | Set-Content $configFile -Encoding UTF8
}

if ($External) {
  Write-Host "外部（学校以外）用のゲームを作成します" -ForegroundColor Magenta
}

if (-not $Name) {
  $Name = Read-Host "新しいゲームの名前（リポジトリ名）"
  if (-not $Name) { throw "名前が空です" }
}

if ($External) {
  # 公開先の GitHub アカウント（.chaso.json に保存され、「GitHub公開(別アカウント)」が使う）
  if (-not $Owner) {
    $def = $memory["ExternalOwner"]
    if ($def) { Write-Host "  Enter のみ: $def" -ForegroundColor DarkGray }
    $answer = (Read-Host "公開先の GitHub アカウント名").Trim()
    $Owner = if ($answer) { $answer } else { $def }
    if (-not $Owner) { throw "GitHub アカウント名が空です" }
  }
  # このリポジトリのコミットに使うメール（任意。GitHub の noreply アドレス推奨）
  if (-not $PSBoundParameters.ContainsKey("Email")) {
    $def = $memory["ExternalEmail"]
    Write-Host "このリポジトリのコミットに使うメールアドレス（GitHub の noreply アドレス推奨）"
    if ($def) {
      Write-Host "  Enter のみ: $def / - : 設定しない（グローバル設定のまま）" -ForegroundColor DarkGray
    } else {
      Write-Host "  Enter のみ: 設定しない（グローバル設定のまま）" -ForegroundColor DarkGray
    }
    $answer = (Read-Host "メール").Trim()
    $Email = if ($answer -eq "-") { "" } elseif ($answer) { $answer } else { $def }
  }
  $memory["ExternalOwner"] = $Owner
  if ($Email) { $memory["ExternalEmail"] = $Email }
  Save-Memory
}

if (-not $Dest) {
  # 初期値: 前回の作成先 → D:\production → ユーザーフォルダ
  $lastDest = $memory["LastDest"]
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
  $memory["LastDest"] = $Dest
  Save-Memory
}
if (-not (Test-Path $Dest)) { New-Item -ItemType Directory -Path $Dest | Out-Null }
Write-Host "作成先: $(Join-Path $Dest $Name)" -ForegroundColor Cyan

$target = Join-Path $Dest $Name
if (Test-Path $target) { throw "$target は既に存在します" }

New-Item -ItemType Directory -Path $target | Out-Null
Set-Location $target

git init -b master | Out-Null
if ($External -and $Email) {
  # このリポジトリだけ別アカウントのコミット情報にする（グローバル設定は変えない）
  git config user.name  $Owner
  git config user.email $Email
}
# subtree add には 1 つ以上のコミットが必要なので、先に README だけコミットする
"# $Name`n`nChasoEngine ベースのゲーム。エンジン本体は project/Chaso（ChasoEngine の subtree）。`n" |
  Set-Content -Encoding UTF8 README.md
git add README.md
git commit -q -m "init: $Name"

Write-Host "==> ChasoEngine を project/Chaso に取り込み" -ForegroundColor Cyan
git subtree add --prefix=project/Chaso $Remote $Branch --squash -m "engine: add ChasoEngine $Branch"

Write-Host "==> テンプレートを展開" -ForegroundColor Cyan
$tpl = Join-Path $target "project\Chaso\Template"
# _root* 以外 → project/ 、 _root/ の中身 → リポジトリ直下（外部用は _root_external/ で上書き）
Get-ChildItem -Path $tpl -Force | Where-Object { $_.Name -notlike "_root*" } | ForEach-Object {
  Copy-Item -Path $_.FullName -Destination (Join-Path $target "project") -Recurse -Force
}
$rootDirs = @("_root")
if ($External) { $rootDirs += "_root_external" }
foreach ($d in $rootDirs) {
  $tplRoot = Join-Path $tpl $d
  if (Test-Path $tplRoot) {
    Get-ChildItem -Path $tplRoot -Force | ForEach-Object {
      Copy-Item -Path $_.FullName -Destination $target -Recurse -Force
    }
  }
}
if ($External) {
  [ordered]@{ mode = "external"; githubOwner = $Owner } | ConvertTo-Json |
    Set-Content (Join-Path $target ".chaso.json") -Encoding UTF8
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
if ($External) {
  Write-Host "  外部用（公開先: $Owner）。VS Code で開くと外部用のボタンが出ます。" -ForegroundColor Magenta
  Write-Host "  GitHub に公開する: 「GitHub公開(別アカウント)」ボタン"
}
Write-Host "  Visual Studio で project\chaso.sln を開いてビルドしてください。"
Write-Host "  エンジンを直したら: .\project\Chaso\scripts\engine-push.ps1"
Write-Host "  エンジンの更新を取り込む: .\project\Chaso\scripts\engine-pull.ps1"
