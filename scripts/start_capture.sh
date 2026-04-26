#!/bin/bash
# Start tcpdump capture for recorder-core v3 testing.
# Captures H.225 (1720), H.245 (1721), GK RAS (1719), control (9001), RTP range (20000-20200).
set -e

IFACE="${IFACE:-ens192}"
OUTDIR="${OUTDIR:-/opt/recorder/logs}"
SUDO_PW="${SUDO_PW:-***REDACTED-PASSWORD***}"

TS=$(date +%Y%m%d_%H%M%S)
PCAP="${OUTDIR}/v3_${TS}.pcap"

# Kill any previous tcpdump we started
echo "$SUDO_PW" | sudo -S -p '' pkill -f "tcpdump.*v3_" 2>/dev/null || true
sleep 1

FILTER='(tcp port 1720 or tcp port 1721 or tcp port 9001) or (udp port 1719) or (udp portrange 20000-20200)'

echo "Starting tcpdump on $IFACE â†?$PCAP"
echo "$SUDO_PW" | sudo -S -p '' nohup tcpdump -i "$IFACE" -s 0 -w "$PCAP" "$FILTER" \
    > /tmp/tcpdump_v3.log 2>&1 &

sleep 2
PID=$(pgrep -f "tcpdump.*${PCAP}" | head -1)
if [ -n "$PID" ]; then
    echo "tcpdump running (PID $PID)"
    echo "pcap: $PCAP"
    ls -la "$PCAP"
else
    echo "tcpdump failed to start"
    cat /tmp/tcpdump_v3.log
    exit 1
fi
