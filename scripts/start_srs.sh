#!/bin/bash
# SRS 启动脚本
SRS_DIR=/opt/recorder/srs

# 检查是否已运行
PID=$(pgrep -f "objs/srs -c")
if [ -n "$PID" ]; then
    echo "SRS already running (PID $PID)"
    exit 0
fi

cd $SRS_DIR
nohup ./objs/srs -c conf/srs.conf > /tmp/srs_start.log 2>&1 &
sleep 1

PID=$(pgrep -f "objs/srs -c")
if [ -n "$PID" ]; then
    echo "SRS started (PID $PID)"
    echo "RTMP:  :1935"
    echo "HTTP:  :8080 (HLS/FLV)"
    echo "API:   :1985 (localhost only)"
    echo "Logs:  $SRS_DIR/objs/srs.log"
else
    echo "SRS failed to start!" >&2
    exit 1
fi
