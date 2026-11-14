$ErrorActionPreference = 'Stop'
$root = Join-Path $HOME 'ESP32-AutoMesh'
$env:IDF_PATH = Join-Path $HOME 'esp\esp-idf'
& (Join-Path $env:IDF_PATH 'export.ps1')
if ($LASTEXITCODE -ne 0) {throw 'ESP-IDF export failed'}
idf.py --version
Set-Location (Join-Path $root 'gateway')
idf.py set-target esp32c3
if ($LASTEXITCODE -ne 0) {throw 'set-target failed'}
idf.py build
if ($LASTEXITCODE -ne 0) {throw 'gateway build failed'}
Write-Output 'AUTOMESH_GATEWAY_BUILD_OK'

