# install_asr.ps1 — sync scripts/asr/ (bridge.py + hotwords + units) to a
# host that already has the sherpa-onnx binary + model, then run
# install_asr.sh remotely.
#
# Usage:
#   .\scripts\install_asr.ps1                       # uses RECORDER_HOST from .env
#   .\scripts\install_asr.ps1 1.2.3.4               # positional override
#   .\scripts\install_asr.ps1 -TargetHost 1.2.3.4 -User deploy
param(
    [string]$TargetHost = "",
    [string]$User       = ""
)
$ErrorActionPreference = "Continue"

. "$PSScriptRoot\load_env.ps1"

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

# ensure remote dest exists and is writable by the deploy user
ssh @sshCommon $target "echo `"$pw`" | sudo -S -p '' mkdir -p /opt/recorder/recorder-core/scripts/asr && echo `"$pw`" | sudo -S -p '' chown -R ${User}:${User} /opt/recorder/recorder-core/scripts/asr"

scp @scpCommon -r "$root\scripts\asr\." "${target}:/opt/recorder/recorder-core/scripts/asr/"
"upload asr=$LASTEXITCODE"

# CRLF strip on the .sh + run installer (.gitattributes locks LF, but
# defensively normalize on the wire just like upload_build.ps1 does)
$cmd = "sed -i 's/\r`$//' /opt/recorder/recorder-core/scripts/asr/install_asr.sh && chmod +x /opt/recorder/recorder-core/scripts/asr/install_asr.sh && RUN_USER='${User}' bash /opt/recorder/recorder-core/scripts/asr/install_asr.sh '$pw'"
ssh @sshCommon $target $cmd
"install=$LASTEXITCODE"
