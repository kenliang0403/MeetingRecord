#!/bin/bash
# stop.sh â€?Gracefully stop recorder-core
PID_FILE="/opt/recorder/run/recorder-core.pid"

if [ ! -f "$PID_FILE" ]; then
    PIDS="$(ps -C recorder-core -o pid= 2>/dev/null | tr -s ' ' | sed 's/^ *//')"
    if [ -z "$PIDS" ]; then
        echo "recorder-core is not running (no PID file)"
        exit 0
    fi
    for PID in $PIDS; do
        if kill -0 "$PID" 2>/dev/null; then
            kill -TERM "$PID"
            echo "Sent SIGTERM to recorder-core (PID $PID)"
        fi
    done
    for i in {1..10}; do
        sleep 1
        ps -C recorder-core >/dev/null 2>&1 || break
    done
    if ps -C recorder-core >/dev/null 2>&1; then
        for PID in $PIDS; do
            kill -KILL "$PID" 2>/dev/null || true
        done
        echo "Force killed"
    fi
    rm -f "$PID_FILE"
    echo "recorder-core stopped"
    exit 0
else
    PID=$(cat "$PID_FILE")
fi

if kill -0 "$PID" 2>/dev/null; then
    kill -TERM "$PID"
    echo "Sent SIGTERM to recorder-core (PID $PID)"
    for i in {1..10}; do
        sleep 1
        kill -0 "$PID" 2>/dev/null || break
    done
    kill -0 "$PID" 2>/dev/null && kill -KILL "$PID" && echo "Force killed"
    rm -f "$PID_FILE"
    echo "recorder-core stopped"
    EXTRA_PIDS="$(ps -C recorder-core -o pid= 2>/dev/null | tr -s ' ' | sed 's/^ *//')"
    if [ -n "$EXTRA_PIDS" ]; then
        for XPID in $EXTRA_PIDS; do
            [ "$XPID" = "$PID" ] && continue
            kill -TERM "$XPID" 2>/dev/null || true
        done
        for i in {1..10}; do
            sleep 1
            ps -C recorder-core >/dev/null 2>&1 || break
        done
        if ps -C recorder-core >/dev/null 2>&1; then
            for XPID in $EXTRA_PIDS; do
                [ "$XPID" = "$PID" ] && continue
                kill -KILL "$XPID" 2>/dev/null || true
            done
            echo "Force killed"
        fi
    fi
else
    echo "recorder-core not running, cleaning up PID file"
    rm -f "$PID_FILE"
    PIDS="$(ps -C recorder-core -o pid= 2>/dev/null | tr -s ' ' | sed 's/^ *//')"
    if [ -n "$PIDS" ]; then
        for PID in $PIDS; do
            kill -TERM "$PID" 2>/dev/null || true
            echo "Sent SIGTERM to recorder-core (PID $PID)"
        done
        for i in {1..10}; do
            sleep 1
            ps -C recorder-core >/dev/null 2>&1 || break
        done
        if ps -C recorder-core >/dev/null 2>&1; then
            for PID in $PIDS; do
                kill -KILL "$PID" 2>/dev/null || true
            done
            echo "Force killed"
        fi
        echo "recorder-core stopped"
    fi
fi
