$ErrorActionPreference = "Stop"

$env:RECORDER_102_PASSWORD = ""
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

$cmd = @"
tail -n 100 /opt/recorder/logs/stdout.log
echo "--- h323trace.log ---"
tail -n 100 /opt/recorder/logs/h323trace.log
"@

& ssh @sshCommon ftadmin@<recorder_host> $cmd | Out-Host
