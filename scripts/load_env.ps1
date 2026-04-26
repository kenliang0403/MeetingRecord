$envFile = Join-Path (Resolve-Path "$PSScriptRoot\..").Path ".env"
if (Test-Path $envFile) {
    Get-Content $envFile | Where-Object { $_ -match '=' -and $_ -notmatch '^\s*#' } | ForEach-Object {
        $name, $value = $_.Split('=', 2)
        [Environment]::SetEnvironmentVariable($name.Trim(), $value.Trim(), "Process")
    }
} else {
    Write-Warning "Could not find .env file at $envFile. Make sure you have created it based on the README."
}
