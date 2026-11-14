$ErrorActionPreference = 'Stop'
$env:IDF_PATH = Join-Path $HOME 'esp\esp-idf'
& (Join-Path $env:IDF_PATH 'export.ps1')
if ($LASTEXITCODE -ne 0) {throw 'IDF environment failed'}
Set-Location (Join-Path $HOME 'ESP32-AutoMesh\gateway')
idf.py reconfigure
if ($LASTEXITCODE -ne 0) {throw 'reconfigure failed'}
idf.py build
if ($LASTEXITCODE -ne 0) {throw 'build failed'}
Write-Output 'AUTOMESH_GATEWAY_BUILD_OK'

