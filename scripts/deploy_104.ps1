$ErrorActionPreference = "Stop"

. "$PSScriptRoot\load_env.ps1"
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
$target = "ftadmin@<recorder_host_secondary>"

# Upload modified sources (do NOT touch config/)
& scp @scpCommon (Join-Path $srcDir "h323\RecorderEndpoint.cpp")   "${target}:/opt/recorder/recorder-core/src/h323/RecorderEndpoint.cpp"   | Out-Host
& scp @scpCommon (Join-Path $srcDir "h323\RecorderConnection.cpp") "${target}:/opt/recorder/recorder-core/src/h323/RecorderConnection.cpp" | Out-Host
& scp @scpCommon (Join-Path $srcDir "h323\RecorderConnection.h")   "${target}:/opt/recorder/recorder-core/src/h323/RecorderConnection.h"   | Out-Host

# Upload deploy script
& scp @scpCommon (Join-Path $PSScriptRoot "deploy_104.sh") "${target}:/tmp/deploy_104.sh" | Out-Host
# Strip CRLF in case Windows git autocrlf snuck through (.gitattributes
# locks *.sh to LF, but defensively also normalize on the wire).
& ssh @sshCommon $target "sed -i 's/\r$//' /tmp/deploy_104.sh" | Out-Host

$pw = $env:RECORDER_102_PASSWORD
& ssh @sshCommon $target "chmod +x /tmp/deploy_104.sh && bash /tmp/deploy_104.sh '$pw'" | Out-Host
