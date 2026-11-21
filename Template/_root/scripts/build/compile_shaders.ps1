$PSScriptRoot = Split-Path -Parent -Path $MyInvocation.MyCommand.Definition
$ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$ShaderDir = Join-Path $ProjectRoot "project\Chaso\Resources\Shader"

Write-Host "🎨 シェーダーのコンパイルを開始します..." -ForegroundColor Cyan

if (-not (Test-Path $ShaderDir)) {
    Write-Host "❌ シェーダーディレクトリが見つかりません: $ShaderDir" -ForegroundColor Red
    exit 1
}

# dxc.exe がパスに通っているか確認
$dxcPath = Get-Command "dxc.exe" -ErrorAction SilentlyContinue
if (-not $dxcPath) {
    # Windows SDKのデフォルトパス等を探すロジック（簡易版）
    $sdkPath = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\dxc.exe" # 代表的なパス
    if (Test-Path $sdkPath) {
        $dxcPath = $sdkPath
    } else {
        Write-Host "⚠️ dxc.exe が見つかりませんでした。PATHが通っているか確認してください。" -ForegroundColor Red
        exit 1
    }
}

$hlslFiles = Get-ChildItem -Path $ShaderDir -Filter "*.hlsl"
$successCount = 0
$errorCount = 0

foreach ($file in $hlslFiles) {
    # 簡易的に、ファイル名に "VS" が含まれれば頂点シェーダー、"PS" ならピクセルシェーダーと推測する
    # ※実際のプロジェクトルールに合わせて変更してください
    $profile = "vs_6_0"
    if ($file.Name -match "PS" -or $file.Name -match "Pixel") { $profile = "ps_6_0" }
    elseif ($file.Name -match "CS" -or $file.Name -match "Compute") { $profile = "cs_6_0" }

    $outputFile = Join-Path $ShaderDir ($file.BaseName + ".cso")
    
    Write-Host "Compiling: $($file.Name) -> $profile" -ForegroundColor Gray
    
    # dxc.exe を呼び出してコンパイル (-T でプロファイル指定, -Fo で出力指定)
    $process = Start-Process -FilePath $dxcPath -ArgumentList "-T $profile", "-Fo `"$outputFile`"", "`"$($file.FullName)`"" -NoNewWindow -Wait -PassThru
    
    if ($process.ExitCode -eq 0) {
        $successCount++
    } else {
        Write-Host "❌ コンパイル失敗: $($file.Name)" -ForegroundColor Red
        $errorCount++
    }
}

Write-Host "✅ シェーダーコンパイル完了 (成功: $successCount, 失敗: $errorCount)" -ForegroundColor Green
if ($errorCount -gt 0) { exit 1 }
