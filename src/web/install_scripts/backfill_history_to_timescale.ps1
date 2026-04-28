$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $scriptRoot

$pythonCandidates = @(
    (Join-Path $scriptRoot ".venv\Scripts\python.exe"),
    (Join-Path (Split-Path -Parent $scriptRoot) ".venv\Scripts\python.exe")
)

$python = $null
foreach ($candidate in $pythonCandidates) {
    if (Test-Path $candidate) {
        $python = $candidate
        break
    }
}

if (-not $python) {
    $python = "python"
}

if (-not $env:OPENEMS_DB_URL) {
    $env:OPENEMS_DB_URL = "postgresql://postgres:postgres@127.0.0.1:5432/openems_admin"
}

$historyDir = if ($env:OPENEMS_HISTORY_DIR) { $env:OPENEMS_HISTORY_DIR } else { (Join-Path (Split-Path -Parent $scriptRoot) "runtime\history") }

Write-Host "Working directory: $scriptRoot"
Write-Host "OPENEMS_DB_URL=$env:OPENEMS_DB_URL"
Write-Host "History directory: $historyDir"
Write-Host "Running history backfill to TimescaleDB..."

& $python ".\install_scripts\backfill_history_to_timescale.py" --history-dir $historyDir