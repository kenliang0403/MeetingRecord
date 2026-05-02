#!/bin/bash
# faststart_existing.sh — 触发 recorder-core 后台对存量 mp4 做 faststart 重写
#
# 等同于在 web 上点一次"重写所有"，但更适合从命令行/cron 触发。
# 进度只在 recorder-core 日志里：
#   journalctl -u recorder-core -f | grep faststart
set -e
echo "Triggering faststart_all on recorder-core (port 9001)..."
python3 /opt/recorder/recorder-core/scripts/ctrl_query.py faststart_all
echo
echo "Watch progress with:"
echo "  journalctl -u recorder-core -f | grep faststart"
