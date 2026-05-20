#!/bin/sh
# SSH_ASKPASS helper. Reads from RECORDER_SSH_PASSWORD (preferred) or
# legacy RECORDER_102_PASSWORD.
if [ -n "$RECORDER_SSH_PASSWORD" ]; then
  printf '%s\n' "$RECORDER_SSH_PASSWORD"
  exit 0
fi
if [ -n "$RECORDER_102_PASSWORD" ]; then
  printf '%s\n' "$RECORDER_102_PASSWORD"
  exit 0
fi
exit 1
