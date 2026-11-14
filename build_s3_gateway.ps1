$ErrorActionPreference = 'Stop'
& "$HOME\esp\esp-idf\export.ps1"
if ($LASTEXITCODE -ne 0) { throw 'IDF export failed' }
$xtensa = "$HOME\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin"
if (-not (Test-Path "$xtensa\xtensa-esp32s3-elf-gcc.exe")) { throw 'ESP32-S3 GCC missing' }
$env:PATH = "$xtensa;$env:PATH"
Set-Location "$HOME\ESP32-AutoMesh\build-targets\esp32s3-4mb\gateway"
idf.py set-target esp32s3
if ($LASTEXITCODE -ne 0) { throw 'S3 target configuration failed' }
Set-Location build
ninja -j 2
if ($LASTEXITCODE -ne 0) { throw 'S3 gateway build failed' }
Write-Output 'AUTOMESH_S3_GATEWAY_BUILT'

