$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptRoot

Write-Host "Working directory: $scriptRoot"
Write-Host "Starting OpenEMS alarm process..."

& ".\bin\openems-alarm.exe" "Local\openems_rt_db" "runtime/alarms_active.json" "config/tables"
