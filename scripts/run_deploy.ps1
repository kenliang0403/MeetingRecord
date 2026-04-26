$ErrorActionPreference = "Stop"

$env:RECORDER_102_PASSWORD = ""
$env:RECORDER_102_SUDO_PASSWORD = ""
& "d:\MeetingRecord\recorder-core\scripts\deploy_to_102.ps1"
