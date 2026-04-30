#!/bin/bash
# Capture all H.323 signaling and RTP between recorder and any MCU IP.
PW="$1"
echo "$PW" | sudo -S -p '' pkill -f 'tcpdump.*smc_close' 2>/dev/null || true
sleep 1
echo "$PW" | sudo -S -p '' rm -f /tmp/smc_close.pcap
echo "$PW" | sudo -S -p '' nohup tcpdump -i any -s 65535 -w /tmp/smc_close.pcap \
    'host <mcu_host> or host <gk_host> or host <gk_host>' \
    >/tmp/tcpd.log 2>&1 &
disown $! || true
sleep 1
pgrep -af tcpdump
ls -la /tmp/smc_close.pcap
