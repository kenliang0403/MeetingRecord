#!/bin/bash
# Smoke-test the field-form save round-trip on a deployed web server.
set -e
echo "=== before ==="
grep -E 'audio_gain|audio_bitrate|terminal_id|alias' /opt/recorder/config/config.json | head -5

echo "=== login ==="
curl -s --max-time 5 -c /tmp/c -X POST http://127.0.0.1:8088/login \
     -d 'username=admin&password=***REDACTED-PASSWORD***' >/dev/null

echo "=== GET /config has expected field names ==="
curl -s --max-time 5 -b /tmp/c http://127.0.0.1:8088/config \
  | grep -oE 'name="(gk_host|gk_alias|audio_gain|terminal_id|stream_enabled|log_level)"' \
  | head -10

echo "=== POST /config/save audio_gain 1.20 ==="
HTTP=$(curl -s --max-time 5 -b /tmp/c -X POST http://127.0.0.1:8088/config/save \
  --data-urlencode 'gk_host=<gk_host>' \
  --data-urlencode 'gk_alias=<alias-1>' \
  --data-urlencode 'gk_password=' \
  --data-urlencode 'terminal_id=TE录播设备' \
  --data-urlencode 'outgoing_dial=<dial-number>' \
  --data-urlencode 'outgoing_mcu=' \
  --data-urlencode 'video_width=1920' \
  --data-urlencode 'video_height=1080' \
  --data-urlencode 'video_fps=30' \
  --data-urlencode 'video_bitrate=2000000' \
  --data-urlencode 'audio_bitrate=128000' \
  --data-urlencode 'audio_gain=1.20' \
  --data-urlencode 'log_level=info' \
  -o /dev/null -w '%{http_code}')
echo "http_code=$HTTP"

echo "=== after ==="
grep -E 'audio_gain|audio_bitrate|terminal_id|alias' /opt/recorder/config/config.json | head -5

echo "=== sanity: rest of JSON intact ==="
python3 - <<'PY'
import json
with open('/opt/recorder/config/config.json') as f:
    cfg = json.load(f)
# Spot-check that fields not on the form are still present
print('streaming.enabled =', cfg.get('streaming',{}).get('enabled'))
print('streaming.rtmp_server =', cfg.get('streaming',{}).get('rtmp_server'))
print('tcp.port =', cfg.get('tcp',{}).get('port'))
print('recorder.audio_sample_rate =', cfg.get('recorder',{}).get('audio_sample_rate'))
print('outgoing.reconnect_delay_s =', cfg.get('outgoing',{}).get('reconnect_delay_s'))
PY
