#!/bin/sh
if [ -n "$RECORDER_102_PASSWORD" ]; then
  printf '%s\n' "$RECORDER_102_PASSWORD"
  exit 0
fi
exit 1
