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

$cmd = @"
cat /tmp/te_tcs_hex.txt | head -n 5
"@

& ssh @sshCommon ftadmin@<recorder_host> $cmd | Out-Host
