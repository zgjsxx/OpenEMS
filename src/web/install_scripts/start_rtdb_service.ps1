$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptRoot

Write-Host "Working directory: $scriptRoot"
if (-not $env:OPENEMS_DB_URL) {
    $env:OPENEMS_DB_URL = "postgresql://postgres:postgres@127.0.0.1:5432/openems_admin"
}

Write-Host "OPENEMS_DB_URL is configured."
Write-Host "Starting OpenEMS RtDb service with PostgreSQL config source..."

& ".\bin\openems-rtdb-service.exe" "postgresql" "config/tables"
