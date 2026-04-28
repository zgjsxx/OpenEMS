$dbUser = "postgres"
$dbName = "openems_admin"

# Auto-detect running PostgreSQL/TimescaleDB container
$container = (docker ps --format "{{.Names}}" | Where-Object { $_ -match "postgres" }) | Select-Object -First 1
if (-not $container) {
    Write-Error "No running PostgreSQL container found. Start one first."
    exit 1
}
$container = $container.Trim()

Write-Host "Creating database '$dbName' in container '$container'..."

$exists = docker exec $container psql -U $dbUser -tAc "SELECT 1 FROM pg_database WHERE datname = '$dbName';"
if ($LASTEXITCODE -ne 0) {
    Write-Error "Failed to query PostgreSQL in container '$container'."
    exit 1
}

$existsText = ""
if ($null -ne $exists) {
    if ($exists -is [System.Array]) {
        $existsText = ($exists -join "").Trim()
    } else {
        $existsText = ([string]$exists).Trim()
    }
}

if ($existsText -eq "1") {
    Write-Host "Database '$dbName' already exists."
} else {
    docker exec $container psql -U $dbUser -c "CREATE DATABASE $dbName;"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Failed to create database '$dbName'."
        exit 1
    }
    Write-Host "Database '$dbName' created."
}

Write-Host ""
Write-Host "Database creation is done."
Write-Host "Tables are created automatically when the admin portal starts."
Write-Host "Next step:"
Write-Host "  .\\src\\web\\start_admin_portal_local.ps1"
