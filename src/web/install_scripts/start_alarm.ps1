$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptRoot

if (-not $env:OPENEMS_DB_URL) {
    $env:OPENEMS_DB_URL = "postgresql://postgres:postgres@127.0.0.1:5432/openems_admin"
}

Write-Host "Working directory: $scriptRoot"
Write-Host "OPENEMS_DB_URL is configured."
Write-Host "Starting OpenEMS alarm process..."

& ".\bin\openems-alarm.exe" "Local\openems_rt_db"
