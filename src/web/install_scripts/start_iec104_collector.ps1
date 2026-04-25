$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptRoot

Write-Host "Working directory: $scriptRoot"
Write-Host "Starting OpenEMS IEC104 collector..."

& ".\bin\openems-iec104-collector.exe" "config/tables"
