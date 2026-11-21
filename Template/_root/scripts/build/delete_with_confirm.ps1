param(
    [Parameter(Mandatory=$true)]
    [string]$TargetFolder,
    [Parameter(Mandatory=$true)]
    [string]$TargetName
)

# Windowsのメッセージボックス用のアセンブリをロード
Add-Type -AssemblyName PresentationFramework

# 確認ダイアログを表示
$msgBoxInput = [System.Windows.MessageBox]::Show("${TargetName} の中身を本当に削除しますか？`n(対象: ${TargetFolder})", "削除の確認", [System.Windows.MessageBoxButton]::YesNo, [System.Windows.MessageBoxImage]::Warning)

if ($msgBoxInput -eq "Yes") {
    $ProjectRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
    $path = Join-Path $ProjectRoot "$TargetFolder\*"
    
    # 削除実行
    Remove-Item -Path $path -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "✅ ${TargetName} を削除しました！" -ForegroundColor Green
} else {
    Write-Host "削除をキャンセルしました。" -ForegroundColor Yellow
}
