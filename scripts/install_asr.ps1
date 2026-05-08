# Upload scripts/asr/ to server and run install_asr.sh
# Usage:  powershell -ExecutionPolicy Bypass -File install_asr.ps1 [<ip>]
#         <ip> defaults to <recorder_host> (only 102 has sherpa + model installed)
$ErrorActionPreference = "Continue"

$root = (Resolve-Path "$PSScriptRoot\..").Path
$ip   = if ($args.Count -gt 0) { $args[0] } else { "<recorder_host>" }

# load password
$envFile = Join-Path $root ".env"
if (Test-Path $envFile) {
    Get-Content $envFile | Where-Object { $_ -match '=' -and $_ -notmatch '^\s*#' } | ForEach-Object {
        $name, $value = $_.Split('=', 2)
        [Environment]::SetEnvironmentVariable($name.Trim(), $value.Trim(), "Process")
    }
}
$pw = $env:RECORDER_102_PASSWORD
if (-not $pw) { Write-Error "RECORDER_102_PASSWORD not set"; exit 1 }

$env:SSH_ASKPASS         = (Join-Path $PSScriptRoot "askpass.cmd")
$env:SSH_ASKPASS_REQUIRE = "force"
$env:DISPLAY             = "1"
$sshCommon = @("-o","StrictHostKeyChecking=no","-o","UserKnownHostsFile=NUL",
               "-o","BatchMode=no",
               "-o","PreferredAuthentications=password,keyboard-interactive",
               "-o","PubkeyAuthentication=no","-o","NumberOfPasswordPrompts=1","-T")
$scpCommon = @("-o","StrictHostKeyChecking=no","-o","UserKnownHostsFile=NUL",
               "-o","BatchMode=no",
               "-o","PreferredAuthentications=password,keyboard-interactive",
               "-o","PubkeyAuthentication=no","-o","NumberOfPasswordPrompts=1")
$target = "ftadmin@$ip"

# ensure remote dest exists and is writable by ftadmin
ssh @sshCommon $target "echo `"$pw`" | sudo -S -p '' mkdir -p /opt/recorder/recorder-core/scripts/asr && echo `"$pw`" | sudo -S -p '' chown -R ftadmin:ftadmin /opt/recorder/recorder-core/scripts/asr"

scp @scpCommon -r "$root\scripts\asr\." "${target}:/opt/recorder/recorder-core/scripts/asr/"
"upload asr=$LASTEXITCODE"

# CRLF strip on the .sh + run installer (.gitattributes locks LF, but
# defensively normalize on the wire just like upload_build.ps1 does)
$cmd = "sed -i 's/\r`$//' /opt/recorder/recorder-core/scripts/asr/install_asr.sh && chmod +x /opt/recorder/recorder-core/scripts/asr/install_asr.sh && bash /opt/recorder/recorder-core/scripts/asr/install_asr.sh '$pw'"
ssh @sshCommon $target $cmd
"install=$LASTEXITCODE"
