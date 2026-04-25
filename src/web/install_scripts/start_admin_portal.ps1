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

if (-not $env:OPENEMS_ADMIN_USERNAME) {
    $env:OPENEMS_ADMIN_USERNAME = "admin"
}

Write-Host "Working directory: $scriptRoot"
Write-Host "OPENEMS_DB_URL=$env:OPENEMS_DB_URL"
Write-Host "Starting OpenEMS admin portal from install root..."

& $python ".\web\run_dashboard.py" --port 8080
