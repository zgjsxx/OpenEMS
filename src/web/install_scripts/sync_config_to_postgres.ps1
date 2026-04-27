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

Write-Host "Working directory: $scriptRoot"
Write-Host "OPENEMS_DB_URL is configured."
Write-Host "Importing config/tables to structured PostgreSQL tables..."

& $python ".\web\sync_config_to_postgres.py" --mode "import" --config-dir "config/tables"
