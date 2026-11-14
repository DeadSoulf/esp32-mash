$ErrorActionPreference = 'Stop'
$root="$HOME\ESP32-AutoMesh"
$log=Join-Path $root "s3-build-status.log"
"Started $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" | Set-Content $log
try {
    & "$HOME\esp\esp-idf\export.ps1" | Out-Null
    $xtensa="$HOME\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin"
    $env:PATH="$xtensa;$env:PATH"
    foreach($role in @("node","gateway")) {
        "BEGIN $role $(Get-Date -Format 'HH:mm:ss')" | Add-Content $log
        $path=Join-Path $root "build-targets\esp32s3-4mb\$role\build"
        Set-Location $path
        & ninja -j 4 2>&1 | Out-File -FilePath (Join-Path $root "s3-$role-ninja.log") -Encoding utf8
        if($LASTEXITCODE -ne 0) { throw "$role compilation error code $LASTEXITCODE" }
        "OK $role $(Get-Date -Format 'HH:mm:ss')" | Add-Content $log
    }
    'ALL_BUILDS_SUCCESS' | Add-Content $log
} catch {
    ("FAILED "+$_.Exception.Message) | Add-Content $log
    exit 1
}

