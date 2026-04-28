# Start the OpenEMS Strategy Engine (Windows PowerShell)
param(
    [string]$ShmName = "Local\openems_rt_db"
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$AppRoot = Resolve-Path "$ScriptDir\.."
$BinDir = "$AppRoot\install\bin"
$ExePath = "$BinDir\openems-strategy-engine.exe"

if (-not (Test-Path $ExePath)) {
    Write-Error "Strategy engine executable not found: $ExePath"
    exit 1
}

if (-not $env:OPENEMS_DB_URL) {
    Write-Error "OPENEMS_DB_URL is not set."
    exit 1
}

Write-Host "[strategy-engine] Starting with shm=$ShmName"
& $ExePath $ShmName
