$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptRoot

Write-Host "Working directory: $scriptRoot"
Write-Host "Starting OpenEMS history sampler..."

& ".\bin\openems-history.exe" "Local\openems_rt_db" "runtime/history" "1000"
