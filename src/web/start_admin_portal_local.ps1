$env:OPENEMS_DB_URL = "postgresql://postgres:postgres@127.0.0.1:5432/openems_admin"
$env:OPENEMS_ADMIN_USERNAME = "admin"

# Optional:
# If you want to override the default seeded admin password, set
# OPENEMS_ADMIN_PASSWORD_HASH before startup.

Write-Host "OPENEMS_DB_URL=$env:OPENEMS_DB_URL"
Write-Host "OPENEMS_ADMIN_USERNAME=$env:OPENEMS_ADMIN_USERNAME"
Write-Host "Starting OpenEMS admin portal on http://localhost:8080/login"

python .\src\web\run_dashboard.py --port 8080
