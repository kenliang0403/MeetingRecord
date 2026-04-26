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

& ssh @scpCommon "ftadmin@<recorder_host>" "tshark -r /tmp/all.pcapng -Y 'h245' -V" > "D:\MeetingRecord\tshark_old_olc.txt"
