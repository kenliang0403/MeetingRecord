#!/bin/bash
# Poll status every 2s for N seconds; log only when state changes.
DUR="${1:-300}"
OUT="${2:-/tmp/status_poll.log}"

: > "$OUT"
prev=""
end=$(( $(date +%s) + DUR ))
while [ $(date +%s) -lt $end ]; do
    now=$(python3 /tmp/ctrl_query.py status 2>/dev/null)
    # Extract the fields we care about to detect state changes
    key=$(echo "$now" | python3 -c "
import sys, json
try:
    d = json.load(sys.stdin).get('data', {})
    print(f\"in_call={d.get('in_call')} token={d.get('call_token')} rec={d.get('recording')} aux={d.get('aux_recording')} h239={d.get('h239_received')} main={d.get('main_file','')} auxf={d.get('aux_file','')}\")
except Exception as e:
    print(f'err:{e}')
")
    if [ "$key" != "$prev" ]; then
        echo "[$(date +%H:%M:%S)] $key" | tee -a "$OUT"
        prev="$key"
    fi
    sleep 2
done
echo "[$(date +%H:%M:%S)] poll ended"
