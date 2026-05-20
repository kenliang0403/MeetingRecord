#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""recorder-asr-bridge.py

Pull recorder-core's main audio stream from local SRS (HTTP-FLV) → decode to
16 kHz mono PCM via ffmpeg → push 100ms float32 chunks to a local
sherpa-onnx-online-websocket-server → append transcript JSONL lines for the
web admin's SSE endpoint and for per-meeting durable storage.

Designed as a long-running systemd service. Survives:
  - SRS stream not present yet (recorder-core not in a call): retries
  - sherpa server restart: reconnects
  - ffmpeg crash: re-spawns

Protocol with sherpa-onnx-online-websocket-server (see sherpa-onnx
online-websocket-client.cc):
  client → server (binary): <num_samples:int32 LE><sample0:f32 LE>...
  client → server (text):   "Done" to flush
  server → client (text):   JSON with {text,tokens,timestamps,segment,...}

Env:
  ASR_SRS_URL   default http://127.0.0.1:8080/live/recorder-main.flv
  ASR_WS_URL    default ws://127.0.0.1:6006
  ASR_RUN_DIR   default /opt/recorder/run
  RECORDINGS_DIR default /opt/recorder/recordings
  RECORDER_CTRL_HOST/PORT  default 127.0.0.1:9001
"""
import asyncio
import json
import logging
import os
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

import websockets


SRS_URL = os.environ.get(
    "ASR_SRS_URL", "http://127.0.0.1:8080/live/recorder-main.flv")
ASR_WS_URL = os.environ.get("ASR_WS_URL", "ws://127.0.0.1:6006")
RUN_DIR = Path(os.environ.get("ASR_RUN_DIR", "/opt/recorder/run"))
RECORDINGS_DIR = Path(os.environ.get(
    "RECORDINGS_DIR", "/opt/recorder/recordings"))
CTRL_HOST = os.environ.get("RECORDER_CTRL_HOST", "127.0.0.1")
CTRL_PORT = int(os.environ.get("RECORDER_CTRL_PORT", "9001"))

SAMPLE_RATE = 16000
CHUNK_SAMPLES = 1600   # 100 ms at 16 kHz
CHUNK_BYTES = CHUNK_SAMPLES * 4   # float32

RECONNECT_DELAY = 3.0

# --- Application-layer watchdog ---------------------------------------
# WS-level ping_interval/ping_timeout only detects TCP/WS death. It does
# NOT detect the case where sherpa's WS is alive but its inference loop
# is stuck and never emits a text message. Real-world bug observed
# 2026-05-13: bridge spawned ffmpeg @13:30:10 then went silent for 2h13m
# while recorder-core's SrsStreamer happily kept publishing — sherpa
# never returned a transcript, recv() blocked forever, the whole session
# hung. Watchdog below cancels the session if no ws message arrives for
# APP_LAYER_TIMEOUT seconds; the outer reconnect loop then tries again.
APP_LAYER_TIMEOUT = float(os.environ.get("ASR_APP_LAYER_TIMEOUT", "60"))
# How often the watchdog logs a heartbeat (so we have observability even
# when nothing's wrong). 0 = only log on timeout.
HEARTBEAT_INTERVAL = float(os.environ.get("ASR_HEARTBEAT_INTERVAL", "30"))

# --- Punctuation (best-effort post-process for finals) ----------------
# Each VAD-final segment is post-processed by spawning a sherpa-onnx
# offline-punctuation subprocess (~0.5s including model load + 20ms infer).
# We append the punctuated version as a *separate* JSONL line marked
# punct=true so the frontend can replace the no-punct line.
PUNCT_BIN = Path(os.environ.get(
    "ASR_PUNCT_BIN",
    "/opt/recorder/asr/tmp/sherpa-onnx-v1.13.0-linux-x64-shared/bin/sherpa-onnx-offline-punctuation"))
PUNCT_MODEL = Path(os.environ.get(
    "ASR_PUNCT_MODEL",
    "/opt/recorder/asr/models/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx"))
SHERPA_LIB = os.environ.get(
    "ASR_SHERPA_LIB",
    "/opt/recorder/asr/tmp/sherpa-onnx-v1.13.0-linux-x64-shared/lib")
PUNCT_TIMEOUT = float(os.environ.get("ASR_PUNCT_TIMEOUT", "8.0"))
PUNCT_ENABLED = PUNCT_BIN.is_file() and PUNCT_MODEL.is_file()


logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("asr-bridge")


_mid_cache_value = None
_mid_cache_ts    = 0.0
_MID_CACHE_TTL_S = 10.0     # 一次会议内 meeting_id 不变，10s 缓存即可

def query_meeting_id():
    """Best-effort one-shot query to recorder-core ControlServer.

    Returns the current meeting_id string or None when no call is active.
    Failure modes (port closed, JSON malformed, timeout) collapse to the
    previously-cached value (better than None on transient ControlServer
    backpressure).

    IMPORTANT — this is a SYNCHRONOUS socket call invoked from inside an
    asyncio coroutine (receiver loop). Calling it 3+/sec on every ws
    message blocks the event loop and the call itself often times out
    when ControlServer is busy processing H.245. Solution: 10-second
    result cache. While the cache is fresh we skip the socket entirely
    and event-loop stays responsive.
    """
    global _mid_cache_value, _mid_cache_ts
    now = time.time()
    if (now - _mid_cache_ts) < _MID_CACHE_TTL_S:
        return _mid_cache_value
    _mid_cache_ts = now   # bump even on failure so we don't hammer
    try:
        with socket.create_connection((CTRL_HOST, CTRL_PORT), timeout=1.5) as s:
            # ControlServer expects a JSON command, not plain text.
            # Sending b"status\n" makes it return {"ok": false} and we
            # never learn the meeting_id — silently failing the
            # per-meeting transcript path. (This was the original bug.)
            s.sendall(b'{"cmd":"status"}\n')
            buf = b""
            while b"\n" not in buf and len(buf) < 65536:
                more = s.recv(4096)
                if not more:
                    break
                buf += more
        line = buf.decode("utf-8", "replace").splitlines()[0]
        d = json.loads(line)
        if d.get("ok") and d.get("data", {}).get("in_call"):
            _mid_cache_value = d["data"].get("meeting_id") or None
        else:
            _mid_cache_value = None
    except Exception as e:
        # keep previous cached value on transient errors rather than
        # dropping to None (and losing per-meeting routing for finals
        # while ControlServer is just briefly busy)
        log.debug("query_meeting_id transient error (keeping cached %r): %r",
                  _mid_cache_value, e)
    return _mid_cache_value


def open_ffmpeg():
    """Spawn ffmpeg pulling SRS FLV → 16 kHz mono float32 PCM on stdout.

    Low-latency flags: nobuffer + low_delay shave ~200ms vs default.
    `-timeout 5000000` (5s) makes ffmpeg exit promptly when SRS has no
    publisher (recorder-core not in a call yet) so the bridge's outer
    loop can reconnect instead of hanging forever.
    Returns subprocess.Popen.
    """
    return subprocess.Popen(
        [
            "ffmpeg",
            "-loglevel", "error",
            "-fflags", "nobuffer",
            "-flags", "low_delay",
            "-rw_timeout", "5000000",
            "-i", SRS_URL,
            "-vn",
            "-ac", "1",
            "-ar", str(SAMPLE_RATE),
            "-f", "f32le",
            "-acodec", "pcm_f32le",
            "-",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
    )


def write_transcript_line(payload):
    """Append one JSONL line to the live tail and (if known) per-meeting file.

    Per-meeting file lets the recordings page replay captions later.
    The live tail is what Flask SSE follows.
    """
    line = json.dumps(payload, ensure_ascii=False) + "\n"
    live = RUN_DIR / "transcript.jsonl"
    try:
        with live.open("a", encoding="utf-8") as f:
            f.write(line)
    except OSError as e:
        log.warning("write live jsonl failed: %s", e)

    mid = payload.get("meeting_id")
    if mid:
        mdir = RECORDINGS_DIR / mid
        if mdir.is_dir():
            try:
                with (mdir / "transcript.jsonl").open("a", encoding="utf-8") as f:
                    f.write(line)
            except OSError as e:
                log.warning("write per-meeting jsonl failed: %s", e)


async def feeder(ws, ff_stdout, state):
    """Pump ffmpeg PCM → sherpa websocket. Blocks until ffmpeg EOF or error.

    state["samples_sent"] is updated cumulatively so the watchdog can show
    "X seconds of audio pushed, Y messages received" heartbeats.
    """
    loop = asyncio.get_event_loop()
    while True:
        # ffmpeg stdout is a blocking pipe; offload reads to a thread
        raw = await loop.run_in_executor(None, ff_stdout.read, CHUNK_BYTES)
        if not raw:
            log.info("ffmpeg stdout EOF")
            return
        n = len(raw) // 4
        if n == 0:
            return
        # sherpa per-message header is num_samples (int32 LE), then float32 LE samples
        await ws.send(struct.pack("<i", n) + raw[: n * 4])
        state["samples_sent"] += n


async def punctuate(text):
    """Spawn sherpa-onnx-offline-punctuation, return punctuated text or None.

    Failure modes (binary missing, model missing, timeout, non-zero exit,
    parse error) all collapse to None — caller falls back to original text.

    Subprocess output looks like:
        ...
        Input text: <original>
        <punctuated>
        Output text:
    so we grab the line after "Input text:".
    """
    if not PUNCT_ENABLED:
        return None
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = SHERPA_LIB + ":" + env.get("LD_LIBRARY_PATH", "")
    try:
        proc = await asyncio.create_subprocess_exec(
            str(PUNCT_BIN),
            f"--ct-transformer={PUNCT_MODEL}",
            "--num-threads=2",
            text,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
            env=env,
        )
    except Exception as e:
        log.warning("punct subprocess spawn failed: %r", e)
        return None
    try:
        stdout, _ = await asyncio.wait_for(
            proc.communicate(), timeout=PUNCT_TIMEOUT)
    except asyncio.TimeoutError:
        try: proc.kill()
        except Exception: pass
        log.warning("punct timeout (%.1fs) — text len=%d", PUNCT_TIMEOUT, len(text))
        return None
    if proc.returncode != 0:
        return None
    out = stdout.decode("utf-8", "replace")
    lines = out.splitlines()
    for i, line in enumerate(lines):
        if "Input text:" in line and i + 1 < len(lines):
            return lines[i + 1].strip()
    return None


async def punct_and_write(text, seg, meeting_id):
    """Fire-and-forget task: punctuate one final and append a new JSONL line.

    Marked with `punct=true` and `replaces_segment=<seg>` so the frontend
    can identify it as a refinement of a previously-shown final and overwrite
    the existing caption.
    """
    punct_text = await punctuate(text)
    if not punct_text or punct_text == text:
        return
    write_transcript_line({
        "t": time.time(),
        "text": punct_text,
        "is_final": True,
        "punct": True,
        "segment": seg,
        "replaces_segment": seg,
        "meeting_id": meeting_id,
    })


async def receiver(ws, state):
    """Consume sherpa JSON messages and persist them.

    sherpa pushes both partial (`is_final=false`) and final (`is_final=true`)
    transcripts. We log both — the SSE consumer in Flask will collapse partials.
    For every final we also schedule an async punctuation task.

    state["last_recv_ts"] is bumped on EVERY incoming ws message (text or
    binary, valid JSON or not). The watchdog uses it to detect application-
    layer silence even when the WS layer is healthy.
    """
    while True:
        try:
            msg = await ws.recv()
        except websockets.ConnectionClosed:
            log.info("sherpa websocket closed")
            return
        # Update before any filtering: any byte from sherpa proves it's alive.
        state["last_recv_ts"] = time.time()
        state["messages_recv"] += 1
        if isinstance(msg, bytes):
            continue   # sherpa never sends binary; ignore defensively
        try:
            d = json.loads(msg)
        except Exception:
            continue
        text = (d.get("text") or "").strip()
        if not text:
            continue
        is_final = bool(d.get("is_final", False))
        seg = int(d.get("segment", 0))
        meeting_id = query_meeting_id()
        payload = {
            "t": time.time(),
            "text": text,
            "is_final": is_final,
            "segment": seg,
            "tokens": d.get("tokens") or [],
            "timestamps": d.get("timestamps") or [],
            "start_time": d.get("start_time"),
            "meeting_id": meeting_id,
        }
        write_transcript_line(payload)
        if is_final:
            state["finals_recv"] += 1
            # Schedule punct in the background; don't block the receiver loop.
            asyncio.create_task(punct_and_write(text, seg, meeting_id))


async def watchdog(state):
    """Periodically print heartbeat; abort session if sherpa goes silent.

    Returns (i.e. participates in the `asyncio.wait FIRST_COMPLETED` of
    session_once) when application-layer silence exceeds APP_LAYER_TIMEOUT,
    triggering the outer reconnect loop to start a fresh session.
    """
    period = max(HEARTBEAT_INTERVAL, 5.0) if HEARTBEAT_INTERVAL > 0 else 10.0
    next_hb = time.time() + HEARTBEAT_INTERVAL
    while True:
        await asyncio.sleep(period)
        now = time.time()
        idle = now - state["last_recv_ts"]
        if idle > APP_LAYER_TIMEOUT:
            log.warning(
                "watchdog: %.0fs no ws message from sherpa "
                "(samples_sent=%d, finals_recv=%d) — killing session",
                idle, state["samples_sent"], state["finals_recv"],
            )
            return
        if HEARTBEAT_INTERVAL > 0 and now >= next_hb:
            audio_s = state["samples_sent"] / SAMPLE_RATE
            log.info(
                "heartbeat: audio_pushed=%.0fs msgs=%d finals=%d idle=%.0fs",
                audio_s, state["messages_recv"],
                state["finals_recv"], idle,
            )
            next_hb = now + HEARTBEAT_INTERVAL


async def session_once():
    """One full ffmpeg ↔ sherpa round-trip. Returns when any of:
       - ffmpeg EOF (publisher stopped / SRS stream gone)
       - sherpa WS connection closed (server died/restarted)
       - watchdog timeout (APP_LAYER_TIMEOUT s no ws msg from sherpa)
    """
    log.info("connecting sherpa @ %s", ASR_WS_URL)
    async with websockets.connect(
        ASR_WS_URL,
        max_size=10_000_000,
        open_timeout=5,
        ping_interval=20,
        ping_timeout=10,
    ) as ws:
        log.info("sherpa connected; spawning ffmpeg for %s", SRS_URL)
        ff = open_ffmpeg()
        # session-local counters shared with feeder/receiver/watchdog
        state = {
            "last_recv_ts":  time.time(),
            "samples_sent":  0,
            "messages_recv": 0,
            "finals_recv":   0,
        }
        try:
            feed_task = asyncio.create_task(feeder(ws, ff.stdout, state))
            recv_task = asyncio.create_task(receiver(ws, state))
            wdog_task = asyncio.create_task(watchdog(state))
            done, pending = await asyncio.wait(
                {feed_task, recv_task, wdog_task},
                return_when=asyncio.FIRST_COMPLETED,
            )
            for t in pending:
                t.cancel()
            try:
                await ws.send("Done")
            except Exception:
                pass
            for t in done:
                exc = t.exception()
                if exc:
                    log.warning("task ended with exception: %r", exc)
        finally:
            try:
                ff.terminate()
                ff.wait(timeout=2)
            except Exception:
                try:
                    ff.kill()
                except Exception:
                    pass
            # surface ffmpeg stderr so SRS-side errors are visible
            try:
                err = ff.stderr.read().decode("utf-8", "replace").strip()
                if err:
                    log.info("ffmpeg stderr: %s", err[:400])
            except Exception:
                pass


async def main():
    RUN_DIR.mkdir(parents=True, exist_ok=True)
    log.info("recorder-asr-bridge starting")
    log.info("  SRS_URL=%s", SRS_URL)
    log.info("  ASR_WS_URL=%s", ASR_WS_URL)
    log.info("  RUN_DIR=%s  RECORDINGS_DIR=%s", RUN_DIR, RECORDINGS_DIR)
    log.info("  PUNCT_ENABLED=%s  PUNCT_MODEL=%s", PUNCT_ENABLED, PUNCT_MODEL)
    log.info("  APP_LAYER_TIMEOUT=%.0fs  HEARTBEAT_INTERVAL=%.0fs",
             APP_LAYER_TIMEOUT, HEARTBEAT_INTERVAL)

    while True:
        try:
            await session_once()
        except (ConnectionRefusedError, OSError, websockets.WebSocketException) as e:
            log.warning("session network error: %r", e)
        except asyncio.CancelledError:
            log.info("cancelled")
            return
        except Exception as e:
            log.exception("unexpected session error: %r", e)
        log.info("reconnecting in %.1fs", RECONNECT_DELAY)
        await asyncio.sleep(RECONNECT_DELAY)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
