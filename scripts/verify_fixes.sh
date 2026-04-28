#!/bin/bash
set +e

echo "=== START status ==="
python3 /tmp/ctrl_query.py status

BEFORE_STDOUT=$(wc -l < /opt/recorder/logs/stdout.log)
echo "=== BEFORE_STDOUT=$BEFORE_STDOUT ==="

echo
echo "=== Test 1: first dial <dial-number> ==="
python3 /tmp/ctrl_query.py dial number=<dial-number>
sleep 6
echo "--- status ---"
python3 /tmp/ctrl_query.py status

echo
echo "=== Test 2: SECOND dial without clear_call (should auto-clear) ==="
python3 /tmp/ctrl_query.py dial number=<dial-number>
sleep 8

echo "--- status (only 1 connection should remain) ---"
python3 /tmp/ctrl_query.py status

echo
echo "=== Test 3: wait for H.239 subscribe timer (5s + 8s = 13s window) ==="
sleep 14
python3 /tmp/ctrl_query.py status

echo
echo "=== new stdout lines ==="
tail -n +$((BEFORE_STDOUT+1)) /opt/recorder/logs/stdout.log

echo
echo "=== highlight: clear / subscribe / H.239 ==="
tail -n +$((BEFORE_STDOUT+1)) /opt/recorder/logs/stdout.log | grep -E -i 'clearing|H\.239|subscribe|extendedVideo|presentationToken|VideoSender|established|cleared'
