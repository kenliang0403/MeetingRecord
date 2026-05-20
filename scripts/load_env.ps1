# load_env.ps1 — read .env from repo root into the current PowerShell process
# environment. Sourced by upload_build.ps1 / upload_web.ps1 / install_asr.ps1 /
# deploy_104.ps1 before any ssh / scp work.
#
# Recognised keys (see .env.example for the full list):
#   RECORDER_HOST              required for any deploy
#   RECORDER_USER              defaults to "ftadmin"
#   RECORDER_SSH_PASSWORD      required when not using key auth
#   RECORDER_HOST_SECONDARY    optional, used by deploy_104.ps1
#
# Legacy compatibility: older versions used RECORDER_102_PASSWORD. If the new
# RECORDER_SSH_PASSWORD is unset but the old one is, we mirror it forward so
# the rest of the scripts can rely on a single name.

$envFile = Join-Path (Resolve-Path "$PSScriptRoot\..").Path ".env"
if (Test-Path $envFile) {
    Get-Content $envFile | Where-Object { $_ -match '=' -and $_ -notmatch '^\s*#' } | ForEach-Object {
        $name, $value = $_.Split('=', 2)
        [Environment]::SetEnvironmentVariable($name.Trim(), $value.Trim(), "Process")
    }
} else {
    Write-Warning "Could not find .env file at $envFile. Copy .env.example to .env and fill in your values."
}

# Default user
if (-not $env:RECORDER_USER) {
    [Environment]::SetEnvironmentVariable("RECORDER_USER", "ftadmin", "Process")
}

# Legacy → new password mapping (one direction only)
if (-not $env:RECORDER_SSH_PASSWORD -and $env:RECORDER_102_PASSWORD) {
    [Environment]::SetEnvironmentVariable("RECORDER_SSH_PASSWORD", $env:RECORDER_102_PASSWORD, "Process")
}
# Mirror forward so askpass.cmd's legacy branch also works during transition
if ($env:RECORDER_SSH_PASSWORD -and -not $env:RECORDER_102_PASSWORD) {
    [Environment]::SetEnvironmentVariable("RECORDER_102_PASSWORD", $env:RECORDER_SSH_PASSWORD, "Process")
}
