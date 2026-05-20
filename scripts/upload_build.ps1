# upload_build.ps1 — sync changed C++ sources to the recorder host, build
# remotely, atomic-install the new binary.
#
# Usage:
#   .\scripts\upload_build.ps1                       # uses RECORDER_HOST from .env
#   .\scripts\upload_build.ps1 -TargetHost 1.2.3.4   # override host
#   .\scripts\upload_build.ps1 -TargetHost 1.2.3.4 -User deploy
#
# The host must already have the recorder-core repo checked out under
# /opt/recorder/recorder-core and the build directory primed by build.sh.
param(
    [string]$TargetHost = "",
    [string]$User       = ""
)

$ErrorActionPreference = "Stop"

. "$PSScriptRoot\load_env.ps1"

if (-not $TargetHost) { $TargetHost = $env:RECORDER_HOST }
if (-not $User)       { $User       = $env:RECORDER_USER }
if (-not $TargetHost) { Write-Error "RECORDER_HOST not set (pass -TargetHost or set in .env)"; exit 1 }
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

$root = (Resolve-Path "$PSScriptRoot\..").Path
$srcDir = Join-Path $root "src"
$target = "${User}@${TargetHost}"

# upload edited sources
& scp @scpCommon (Join-Path $srcDir "h323\RecorderEndpoint.cpp")   "${target}:/opt/recorder/recorder-core/src/h323/RecorderEndpoint.cpp"   | Out-Host
& scp @scpCommon (Join-Path $srcDir "h323\RecorderConnection.cpp") "${target}:/opt/recorder/recorder-core/src/h323/RecorderConnection.cpp" | Out-Host
& scp @scpCommon (Join-Path $srcDir "h323\RecorderConnection.h")   "${target}:/opt/recorder/recorder-core/src/h323/RecorderConnection.h"   | Out-Host

# upload redeploy script and strip CRLF in case git autocrlf converted it
& scp @scpCommon (Join-Path $PSScriptRoot "redeploy.sh") "${target}:/tmp/redeploy.sh" | Out-Host
& ssh @sshCommon $target "sed -i 's/\r$//' /tmp/redeploy.sh" | Out-Host

# build & redeploy on remote
$pw = $env:RECORDER_SSH_PASSWORD
$cmd = "cd /opt/recorder/recorder-core/build && echo `"$pw`" | sudo -S -p '' cmake --build . --target recorder-core -j 2>&1 | tail -25 && bash /tmp/redeploy.sh `"$pw`""
& ssh @sshCommon $target $cmd | Out-Host
