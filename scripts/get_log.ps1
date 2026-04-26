$ErrorActionPreference = "Stop"

$server = "ftadmin@<recorder_host>"
$remoteBase = "/opt/recorder"

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

$localPath = "D:\MeetingRecord\recorder-core\logs\h323trace.log"
if (-not (Test-Path "D:\MeetingRecord\recorder-core\logs")) {
    New-Item -ItemType Directory -Force -Path "D:\MeetingRecord\recorder-core\logs" | Out-Null
}

& scp @scpCommon "${server}:${remoteBase}/logs/h323trace.log" $localPath | Out-Host
Write-Host "Log downloaded to $localPath"
