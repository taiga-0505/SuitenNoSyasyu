<#
.SYNOPSIS
  エンジンのリソース (Chaso/Resources) とゲームの Resources を同期する。ビルド前に自動実行される。

.DESCRIPTION
  実行ファイルは project/Resources/ ひとつだけを見る（従来どおり）。
  エンジン側のリソースは ChasoEngine リポジトリ内の Chaso/Resources/ にあるので、
  ビルド前にこのスクリプトが両者を「新しい方が勝つ」ルールで同期する:

    1. Chaso/Resources → Resources   : 新しいファイル・更新されたファイルをコピー（無いものは追加）
    2. Resources → Chaso/Resources   : エンジン側に「既にある」ファイルだけ、更新されていれば書き戻す
                                       （ゲーム固有のファイルはエンジン側に混ざらない）
    3. Resources/.gitignore を自動生成: エンジン由来のファイルをゲームリポジトリでは追跡しない

  したがって、エンジンのシェーダーやフォントは project/Resources/ で直接編集してよい。
  次のビルドで Chaso/Resources に書き戻され、engine-push.ps1 で ChasoEngine に反映される。
  新しいファイルをエンジンに追加したいときは Chaso/Resources/ に置く（Resources/ に置くとゲーム側扱い）。

.PARAMETER ProjectDir
  project/ フォルダ（chaso.sln のある場所）。省略時はこのスクリプトから相対で求める。
#>
param(
  [string]$ProjectDir = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
)
$ErrorActionPreference = "Stop"

$engine = Join-Path $ProjectDir "Chaso\Resources"
$game   = Join-Path $ProjectDir "Resources"
if (-not (Test-Path $engine)) { Write-Host "[sync-resources] skip: $engine が無い"; exit 0 }
if (-not (Test-Path $game))   { New-Item -ItemType Directory -Path $game | Out-Null }

# 同期しないもの（ビルド生成物）
$exclude = @("pso_cache.bin", "desktop.ini", "Thumbs.db", ".gitignore")
$xf = $exclude | ForEach-Object { $_ }

function Run-Robocopy([string[]]$rcArgs) {
  & robocopy @rcArgs | Out-Null
  # robocopy の終了コード: 0-7 は成功（1 = コピーあり）、8 以上が失敗
  if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE): $($rcArgs -join ' ')" }
}

# 1. エンジン → ゲーム（新しい方が勝つ / 無いものは追加）
Run-Robocopy (@($engine, $game, "/E", "/XO", "/XF") + $xf + @("/NJH", "/NJS", "/NDL", "/NFL", "/NP", "/R:1", "/W:1"))

# 2. ゲーム → エンジン（エンジン側に既にあるファイルのみ / 新しい方が勝つ）
#    /XL = 宛先に無いファイル(lonely)はコピーしない → ゲーム固有ファイルは混ざらない
Run-Robocopy (@($game, $engine, "/E", "/XO", "/XL", "/XF") + $xf + @("/NJH", "/NJS", "/NDL", "/NFL", "/NP", "/R:1", "/W:1"))

# 3. Resources/.gitignore を生成（エンジン由来のファイルはゲームリポジトリで追跡しない）
$engineRoot = (Resolve-Path $engine).Path.TrimEnd('\') + '\'
$header = @(
  "# 自動生成（Chaso/scripts/sync-resources.ps1）: 編集しないこと",
  "# ここに並ぶファイルはエンジン (Chaso/Resources) 由来のコピーで、ChasoEngine 側で管理される",
  ".gitignore"
)
$body = @(Get-ChildItem -Path $engine -Recurse -File |
  Where-Object { $exclude -notcontains $_.Name } |
  ForEach-Object {
    $rel = $_.FullName.Substring($engineRoot.Length).Replace('\', '/')
    # gitignore の特殊文字をエスケープ
    $rel = $rel -replace '([\[\]#!*?])', '\$1'
    "/" + $rel
  } | Sort-Object)
$content = (($header + $body) -join "`n") + "`n"
$target = Join-Path $game ".gitignore"
$old = if (Test-Path $target) { Get-Content $target -Raw -Encoding UTF8 } else { "" }
if ($old -ne $content) {
  [System.IO.File]::WriteAllText($target, $content, (New-Object System.Text.UTF8Encoding($false)))
}

Write-Host "[sync-resources] OK  (Chaso/Resources <-> Resources, $($body.Count) engine files)"
exit 0
