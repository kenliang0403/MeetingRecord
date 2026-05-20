# deploy_104.ps1 — same flow as upload_build.ps1 but targets the secondary
# recorder host. The `_104` in the name is historical; the actual host is
# read from RECORDER_HOST_SECONDARY (or -TargetHost), see .env.example.
#
# Usage:
#   .\scripts\deploy_104.ps1                          # uses RECORDER_HOST_SECONDARY
#   .\scripts\deploy_104.ps1 -TargetHost 1.2.3.4      # override
param(
    [string]$TargetHost = "",
    [string]$User       = ""
)
$ErrorActionPreference = "Stop"

. "$PSScriptRoot\load_env.ps1"

if (-not $TargetHost) { $TargetHost = $env:RECORDER_HOST_SECONDARY }
if (-not $TargetHost) { $TargetHost = $env:RECORDER_HOST }    # fallback to primary
if (-not $User)       { $User       = $env:RECORDER_USER }
if (-not $TargetHost) { Write-Error "RECORDER_HOST_SECONDARY (or RECORDER_HOST) not set"; exit 1 }
if (-not $User)       { Write-Error "RECORDER_USER not set"; exit 1 }
if (-not $env:RECORDER_SSH_PASSWORD) { Write-Error "RECORDER_SSH_PASSWORD not set (see .env.example)"; exit 1 }

$env:SSH_ASKPASS = (Join-Path $PSScriptRoot "askpass.cmd")
$env:SSH_ASKPASS_REQUIRE = "force"
$env:DISPLAY = "1"

$sshCommon = @(
  "-o", "StrictHostKeyChecking=no",
  "-o", "UserKnownHostsFile=NUL",
  "-o", "BatchMode=no",
  "-o", "PreferredAuthentications=password,keyboard-interactive",
  "-o", "PubkeyAuthentication=no",
  "-o", "NumberOfPasswordPrompts=1",
  "-T"
)
$scpCommon = @(
  "-o", "StrictHostKeyChecking=no",
  "-o", "UserKnownHostsFile=NUL",
  "-o", "BatchMode=no",
  "-o", "PreferredAuthentications=password,keyboard-interactive",
  "-o", "PubkeyAuthentication=no",
  "-o", "NumberOfPasswordPrompts=1"
)

$root   = (Resolve-Path "$PSScriptRoot\..").Path
$srcDir = Join-Path $root "src"
$target = "${User}@${TargetHost}"

# Upload modified sources (do NOT touch config/)
& scp @scpCommon (Join-Path $srcDir "h323\RecorderEndpoint.cpp")   "${target}:/opt/recorder/recorder-core/src/h323/RecorderEndpoint.cpp"   | Out-Host
& scp @scpCommon (Join-Path $srcDir "h323\RecorderConnection.cpp") "${target}:/opt/recorder/recorder-core/src/h323/RecorderConnection.cpp" | Out-Host
& scp @scpCommon (Join-Path $srcDir "h323\RecorderConnection.h")   "${target}:/opt/recorder/recorder-core/src/h323/RecorderConnection.h"   | Out-Host

# Upload deploy script
& scp @scpCommon (Join-Path $PSScriptRoot "deploy_104.sh") "${target}:/tmp/deploy_104.sh" | Out-Host
# Strip CRLF in case Windows git autocrlf snuck through (.gitattributes
# locks *.sh to LF, but defensively also normalize on the wire).
& ssh @sshCommon $target "sed -i 's/\r$//' /tmp/deploy_104.sh" | Out-Host

$pw = $env:RECORDER_SSH_PASSWORD
& ssh @sshCommon $target "chmod +x /tmp/deploy_104.sh && RUN_USER='${User}' bash /tmp/deploy_104.sh '$pw'" | Out-Host
