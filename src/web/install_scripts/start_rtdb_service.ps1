$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptRoot

Write-Host "Working directory: $scriptRoot"
Write-Host "Starting OpenEMS RtDb service..."

& ".\bin\openems-rtdb-service.exe" "config/tables"
