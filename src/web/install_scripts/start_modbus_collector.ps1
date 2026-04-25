$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptRoot

Write-Host "Working directory: $scriptRoot"
Write-Host "Starting OpenEMS Modbus collector..."

& ".\bin\openems-modbus-collector.exe" "config/tables"
