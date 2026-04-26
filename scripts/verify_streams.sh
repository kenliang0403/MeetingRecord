#!/bin/bash
sleep 5
echo '--- HTTP-FLV main (6s timeout) ---'
timeout 6 curl -sS -o /tmp/main.flv -w 'HTTP=%{http_code} SIZE=%{size_download}B SPEED=%{speed_download}B/s\n' http://127.0.0.1:8080/live/recorder-main.flv 2>&1
ls -la /tmp/main.flv 2>&1

echo ''
echo '--- HLS main ---'
for i in 1 2 3; do
  R=$(curl -sS -w ' HTTP=%{http_code}' http://127.0.0.1:8080/live/recorder-main.m3u8)
  echo "attempt $i: $R"
  sleep 2
done

echo ''
echo '--- HLS dir contents ---'
ls -la /opt/recorder/srs/objs/nginx/html/live/ 2>&1 | head

echo ''
echo '--- Streams kbps ---'
curl -s http://127.0.0.1:1985/api/v1/streams/ > /tmp/streams.json
python3 -c "
import json
d = json.load(open('/tmp/streams.json'))
for s in d['streams']:
    print(f\"{s['name']}: recv_30s={s['kbps']['recv_30s']}kbps recv_bytes={s['recv_bytes']} publish={s['publish']['active']} clients={s['clients']}\")
"

echo ''
echo '--- SRS error events last 30 lines ---'
grep -iE 'SecurityDeny|publish timeout|ERROR' /opt/recorder/srs/objs/srs.log | tail -10
