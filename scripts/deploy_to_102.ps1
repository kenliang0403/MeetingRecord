$ErrorActionPreference = "Stop"

$server = "ftadmin@<recorder_host>"
$remoteBase = "/opt/recorder"

$sshPw = $env:RECORDER_102_PASSWORD
if ([string]::IsNullOrWhiteSpace($sshPw)) {
  throw "Missing env var: RECORDER_102_PASSWORD"
}

$root = Split-Path -Parent $PSScriptRoot

function Escape-BashSingleQuoted([string]$s) {
  return $s -replace "'", "'\\''"
}

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

$sudoPw = $env:RECORDER_102_SUDO_PASSWORD
if ([string]::IsNullOrWhiteSpace($sudoPw)) {
  $sudoPw = $sshPw
}
$sudoPwEsc = Escape-BashSingleQuoted $sudoPw
$cmd = @"
echo '$sudoPwEsc' | sudo -S mkdir -p $remoteBase/{bin,config,logs,recordings,run,recorder-core,tarballs,third_party/nlohmann} &&
echo '$sudoPwEsc' | sudo -S chown -R ftadmin:ftadmin $remoteBase &&
mkdir -p $remoteBase/recorder-core/scripts
"@

& ssh @sshCommon $server $cmd | Out-Host

& scp @scpCommon -r (Join-Path $root "src") "${server}:$remoteBase/recorder-core/" | Out-Host
& scp @scpCommon (Join-Path $root "CMakeLists.txt") "${server}:$remoteBase/recorder-core/" | Out-Host
& scp @scpCommon -r (Join-Path $root "config") "${server}:$remoteBase/recorder-core/" | Out-Host
& scp @scpCommon (Join-Path $root "config\config.json") "${server}:$remoteBase/config/" | Out-Host
& scp @scpCommon (Join-Path $root "scripts\\*.sh") "${server}:$remoteBase/recorder-core/scripts/" | Out-Host

$cmd2 = @"
chmod +x $remoteBase/recorder-core/scripts/*.sh &&
bash $remoteBase/recorder-core/scripts/stop.sh || true
"@
& ssh @sshCommon $server $cmd2 | Out-Host

$cmd3 = @"
cd $remoteBase/recorder-core &&
export RECORDER_102_SUDO_PASSWORD='$sudoPwEsc' &&
bash scripts/build.sh
"@
& ssh @sshCommon $server $cmd3 | Out-Host

$cmd4 = @"
bash $remoteBase/recorder-core/scripts/start.sh
"@
& ssh @sshCommon $server $cmd4 | Out-Host
