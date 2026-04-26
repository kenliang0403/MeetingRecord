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

$sudoPwEsc = ""

$cmd = @"
echo '$sudoPwEsc' | sudo -S tcpdump -i any tcp port 1720 -c 100 -w /tmp/h245_debug.pcap &
sleep 15
echo '$sudoPwEsc' | sudo -S killall tcpdump
tshark -r /tmp/h245_debug.pcap -d tcp.port==1720,tpkt -Y "h245.OpenLogicalChannelAck" -T text -V > /tmp/h245_olc_ack.txt
cat /tmp/h245_olc_ack.txt | grep -A 20 -i "mediaControlChannel" || echo "No media port info"
"@

& ssh @sshCommon ftadmin@<recorder_host> $cmd | Out-Host
