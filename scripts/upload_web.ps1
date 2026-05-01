# Upload web/ to server and run install_web.sh
# Usage:  powershell -ExecutionPolicy Bypass -File upload_web.ps1 <ip>
#         <ip> defaults to <recorder_host> (102 box)
$ErrorActionPreference = "Continue"

$root = (Resolve-Path "$PSScriptRoot\..").Path
$ip   = if ($args.Count -gt 0) { $args[0] } else { "<recorder_host>" }

# load password
$envFile = Join-Path $root ".env"
if (Test-Path $envFile) {
    Get-Content $envFile | Where-Object { $_ -match '=' -and $_ -notmatch '^\s*#' } | ForEach-Object {
        $name, $value = $_.Split('=', 2)
        [Environment]::SetEnvironmentVariable($name.Trim(), $value.Trim(), "Process")
    }
}
$pw = $env:RECORDER_102_PASSWORD
if (-not $pw) { Write-Error "RECORDER_102_PASSWORD not set"; exit 1 }

$env:SSH_ASKPASS         = (Join-Path $PSScriptRoot "askpass.cmd")
$env:SSH_ASKPASS_REQUIRE = "force"
$env:DISPLAY             = "1"
$sshCommon = @("-o","StrictHostKeyChecking=no","-o","UserKnownHostsFile=NUL",
               "-o","BatchMode=no",
               "-o","PreferredAuthentications=password,keyboard-interactive",
               "-o","PubkeyAuthentication=no","-o","NumberOfPasswordPrompts=1","-T")
$scpCommon = @("-o","StrictHostKeyChecking=no","-o","UserKnownHostsFile=NUL",
               "-o","BatchMode=no",
               "-o","PreferredAuthentications=password,keyboard-interactive",
               "-o","PubkeyAuthentication=no","-o","NumberOfPasswordPrompts=1")
$target = "ftadmin@$ip"

# 1) ensure remote dest exists, then scp the whole web/
ssh @sshCommon $target "echo `"$pw`" | sudo -S -p '' mkdir -p /opt/recorder/recorder-core/web /opt/recorder/recorder-core/scripts && echo `"$pw`" | sudo -S -p '' chown -R ftadmin:ftadmin /opt/recorder/recorder-core/web /opt/recorder/recorder-core/scripts"

scp @scpCommon -r "$root\web\." "${target}:/opt/recorder/recorder-core/web/"
"upload web=$LASTEXITCODE"
scp @scpCommon "$root\scripts\install_web.sh" "${target}:/opt/recorder/recorder-core/scripts/install_web.sh"
"upload script=$LASTEXITCODE"

# 2) run installer (CRLF strip + chmod first)
$cmd = "sed -i 's/\r`$//' /opt/recorder/recorder-core/scripts/install_web.sh && chmod +x /opt/recorder/recorder-core/scripts/install_web.sh && bash /opt/recorder/recorder-core/scripts/install_web.sh '$pw'"
ssh @sshCommon $target $cmd
"install=$LASTEXITCODE"
