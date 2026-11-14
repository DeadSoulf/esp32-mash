$ErrorActionPreference = 'Stop'
$env:IDF_PATH = Join-Path $HOME 'esp\esp-idf'
& (Join-Path $env:IDF_PATH 'export.ps1')
if ($LASTEXITCODE -ne 0) {throw 'IDF environment failed'}
Set-Location (Join-Path $HOME 'ESP32-AutoMesh\node')
idf.py set-target esp32c3
if ($LASTEXITCODE -ne 0) {throw 'set-target failed'}
idf.py build
if ($LASTEXITCODE -ne 0) {throw 'node build failed'}
Write-Output 'AUTOMESH_NODE_BUILD_OK'

