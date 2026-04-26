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

$sudoPwEsc = ""

$cmd = @"
echo '$sudoPwEsc' | sudo -S mkdir -p /usr/local/lib/pwlib/codecs/audio
echo '$sudoPwEsc' | sudo -S cp /tmp/recorder_build/h323/h323plus-1_27_2/plugins/audio/G722/g722_audio_pwplugin.so /usr/local/lib/pwlib/codecs/audio/
echo '$sudoPwEsc' | sudo -S mkdir -p /usr/lib/pwlib/codecs/audio
echo '$sudoPwEsc' | sudo -S cp /tmp/recorder_build/h323/h323plus-1_27_2/plugins/audio/G722/g722_audio_pwplugin.so /usr/lib/pwlib/codecs/audio/
"@

& ssh @sshCommon ftadmin@<recorder_host> $cmd | Out-Host
