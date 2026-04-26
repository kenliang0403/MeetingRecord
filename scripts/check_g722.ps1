$ErrorActionPreference = "Stop"
$env:RECORDER_102_PASSWORD = ""
$env:SSH_ASKPASS = (Join-Path $PSScriptRoot "askpass.cmd")
$env:SSH_ASKPASS_REQUIRE = "force"
$env:DISPLAY = "1"

$scpCommon = @(
  "-o", "StrictHostKeyChecking=no",
  "-o", "UserKnownHostsFile=NUL",
  "-o", "BatchMode=no",
  "-o", "PreferredAuthentications=password,keyboard-interactive",
  "-o", "PubkeyAuthentication=no",
  "-o", "NumberOfPasswordPrompts=1"
)

& ssh @scpCommon "ftadmin@<recorder_host>" "echo '***REDACTED-PASSWORD***' | sudo -S find /usr/local/lib/pwlib /usr/local/lib/ptlib /usr/lib/pwlib /usr/lib/ptlib -type f -name '*g722*' 2>/dev/null; echo '---'; find / -type d -name 'ptlib' -o -name 'h323plus' 2>/dev/null | grep -v 'sys' | grep -v 'proc'"
