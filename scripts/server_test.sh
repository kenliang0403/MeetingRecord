#!/bin/bash
# Server-side test: two dial+hangup cycles, mirror SMC test pattern.
set -u

ts() { date '+%Y-%m-%d %H:%M:%S'; }
log() { echo "[$(ts)] $*"; }

NUM="${1:-<dial-number>}"
DWELL="${2:-25}"     # seconds between dial and clear_call
GAP="${3:-5}"        # seconds between two cycles

log "=== Server-side test: dial $NUM (dwell=${DWELL}s, gap=${GAP}s) ==="
log "--- baseline status ---"
python3 /tmp/ctrl_query.py status
echo

# ── Cycle 1 ──────────────────────────────────────────────
log ">>> Cycle 1: dial $NUM"
python3 /tmp/ctrl_query.py dial number=$NUM
sleep 3
log "--- status +3s ---"
python3 /tmp/ctrl_query.py status
sleep $((DWELL - 3))
log "--- status just before clear ---"
python3 /tmp/ctrl_query.py status
log ">>> Cycle 1: clear_call"
python3 /tmp/ctrl_query.py clear_call
sleep 3
log "--- status after clear ---"
python3 /tmp/ctrl_query.py status
echo

sleep $GAP

# ── Cycle 2 ──────────────────────────────────────────────
log ">>> Cycle 2: dial $NUM"
python3 /tmp/ctrl_query.py dial number=$NUM
sleep 3
log "--- status +3s ---"
python3 /tmp/ctrl_query.py status
sleep $((DWELL - 3))
log "--- status just before clear ---"
python3 /tmp/ctrl_query.py status
log ">>> Cycle 2: clear_call"
python3 /tmp/ctrl_query.py clear_call
sleep 3
log "--- status after clear ---"
python3 /tmp/ctrl_query.py status

log "=== Test complete ==="
