# upload_web.ps1 — sync web/ and install_web.sh to a recorder host, then
# run install_web.sh remotely (smart restart — see install_web.sh).
#
# Usage:
#   .\scripts\upload_web.ps1                       # uses RECORDER_HOST from .env
#   .\scripts\upload_web.ps1 1.2.3.4               # positional override for host
#   .\scripts\upload_web.ps1 -TargetHost 1.2.3.4 -User deploy
param(
    [string]$TargetHost = "",
    [string]$User       = ""
)
$ErrorActionPreference = "Continue"

. "$PSScriptRoot\load_env.ps1"

# Positional fallback for legacy callers: `upload_web.ps1 <ip>`
if (-not $TargetHost -and $args.Count -gt 0) { $TargetHost = $args[0] }
if (-not $TargetHost) { $TargetHost = $env:RECORDER_HOST }
if (-not $User)       { $User       = $env:RECORDER_USER }
if (-not $TargetHost) { Write-Error "RECORDER_HOST not set (pass -TargetHost or set in .env)"; exit 1 }
if (-not $User)       { Write-Error "RECORDER_USER not set"; exit 1 }

$pw = $env:RECORDER_SSH_PASSWORD
if (-not $pw) { Write-Error "RECORDER_SSH_PASSWORD not set (see .env.example)"; exit 1 }

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
$root   = (Resolve-Path "$PSScriptRoot\..").Path
$target = "${User}@${TargetHost}"

# 1) ensure remote dest exists, then scp the whole web/
ssh @sshCommon $target "echo `"$pw`" | sudo -S -p '' mkdir -p /opt/recorder/recorder-core/web /opt/recorder/recorder-core/scripts && echo `"$pw`" | sudo -S -p '' chown -R ${User}:${User} /opt/recorder/recorder-core/web /opt/recorder/recorder-core/scripts"

scp @scpCommon -r "$root\web\." "${target}:/opt/recorder/recorder-core/web/"
"upload web=$LASTEXITCODE"
scp @scpCommon "$root\scripts\install_web.sh" "${target}:/opt/recorder/recorder-core/scripts/install_web.sh"
"upload script=$LASTEXITCODE"

# 2) run installer (CRLF strip + chmod first). RUN_USER tells install_web.sh
#    which uid:gid to chown things to; defaults to ftadmin inside the script.
$cmd = "sed -i 's/\r`$//' /opt/recorder/recorder-core/scripts/install_web.sh && chmod +x /opt/recorder/recorder-core/scripts/install_web.sh && RUN_USER='${User}' bash /opt/recorder/recorder-core/scripts/install_web.sh '$pw'"
ssh @sshCommon $target $cmd
"install=$LASTEXITCODE"
