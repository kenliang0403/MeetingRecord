#!/bin/bash
# Run: bash analyze_pcap.sh <pcap> <start_time> <end_time>
# Lists all H.245 messages between two times.
PCAP="$1"
START="$2"
END="$3"
tshark -r "$PCAP" -t ad \
  -d 'tcp.port==11739,h245' -d 'tcp.port==49567,h245' \
  -d 'tcp.port==11738,h245' -d 'tcp.port==36635,h245' \
  -Y "(frame.time >= \"$START\") && (frame.time <= \"$END\") && !rtp && !rtcp && tcp.len > 0" \
  2>/dev/null
