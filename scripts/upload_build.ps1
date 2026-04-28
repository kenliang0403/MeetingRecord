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

# upload edited sources
& scp @scpCommon (Join-Path $srcDir "h323\RecorderEndpoint.cpp") ftadmin@<recorder_host>:/opt/recorder/recorder-core/src/h323/RecorderEndpoint.cpp | Out-Host
& scp @scpCommon (Join-Path $srcDir "h323\RecorderConnection.cpp") ftadmin@<recorder_host>:/opt/recorder/recorder-core/src/h323/RecorderConnection.cpp | Out-Host

# upload redeploy script
& scp @scpCommon (Join-Path $PSScriptRoot "redeploy.sh") ftadmin@<recorder_host>:/tmp/redeploy.sh | Out-Host

# build & redeploy on remote
$pw = $env:RECORDER_102_PASSWORD
$cmd = "cd /opt/recorder/recorder-core/build && echo `"$pw`" | sudo -S -p '' cmake --build . --target recorder-core -j 2>&1 | tail -25 && bash /tmp/redeploy.sh `"$pw`""
& ssh @sshCommon ftadmin@<recorder_host> $cmd | Out-Host
