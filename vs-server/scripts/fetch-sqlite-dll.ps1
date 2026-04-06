# 从 sqlite.org 下载官方 sqlite-dll-win-x64 zip，解压出 sqlite3.dll 到 third_party/sqlite/
# 版本号请对照 https://www.sqlite.org/download.html 中「Precompiled Binaries for Windows」一行，
# 并同步修改下方 $Version 与 $Year（路径形如 https://www.sqlite.org/{Year}/sqlite-dll-win-x64-{Version}.zip）。
$ErrorActionPreference = 'Stop'
$Version = '3510300'
$Year = '2026'
$Url = "https://www.sqlite.org/$Year/sqlite-dll-win-x64-$Version.zip"

$Root = Split-Path -Parent $PSScriptRoot
$SqliteDir = Join-Path $Root 'third_party' 'sqlite'
$ZipPath = Join-Path $SqliteDir "sqlite-dll-win-x64-$Version.zip"
New-Item -ItemType Directory -Force -Path $SqliteDir | Out-Null

Write-Host "Downloading $Url ..."
curl.exe -L --fail --retry 5 --retry-delay 3 --retry-all-errors -o $ZipPath $Url

$Extract = Join-Path $SqliteDir '_extract_dll_tmp'
if (Test-Path $Extract) {
    Remove-Item -Recurse -Force $Extract
}
New-Item -ItemType Directory -Force -Path $Extract | Out-Null
Expand-Archive -LiteralPath $ZipPath -DestinationPath $Extract -Force

$Dll = Get-ChildItem -Path $Extract -Filter sqlite3.dll -Recurse | Select-Object -First 1
if (-not $Dll) {
    throw 'zip 中未找到 sqlite3.dll'
}
Copy-Item -LiteralPath $Dll.FullName -Destination (Join-Path $SqliteDir 'sqlite3.dll') -Force
Remove-Item -Recurse -Force $Extract

Get-Item (Join-Path $SqliteDir 'sqlite3.dll') | Format-List FullName, Length, LastWriteTime
Write-Host '完成。重新生成 vs-server（x64）会将 DLL 复制到输出目录。'
