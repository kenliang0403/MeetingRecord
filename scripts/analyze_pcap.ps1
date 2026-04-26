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

& ssh @scpCommon "ftadmin@<recorder_host>" "tshark -r /tmp/9000820595.pcap -Y 'h245' -x" > "D:\MeetingRecord\tshark_tcs_hex.txt"
