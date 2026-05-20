#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""asr_offline.py — rescue / batch transcribe an existing meeting mp4.

Reads /opt/recorder/recordings/<meeting_id>/main_NN.mp4 via ffmpeg, pushes
PCM at max speed to the local sherpa-onnx-online-websocket-server, collects
finals, and writes them to <meeting_dir>/transcript.jsonl using the
meeting's wall_start_ms as the absolute timeline anchor so player.js can
align captions with mp4 currentTime.

This is the "rescue" path for meetings whose live bridge missed audio for
any reason (timeout, hang, bridge restart, etc.).

Usage:
    python3 asr_offline.py <meeting_id> [--mp4 main_02.mp4] [--overwrite]

Examples:
    python3 asr_offline.py 20260513_820686
    python3 asr_offline.py 20260513_820686 --overwrite
    python3 asr_offline.py 20260513_820686 --mp4 main_02.mp4

Defaults to processing main_01.mp4 (or first main_*.mp4 if 01 missing).
Set ASR_OFFLINE_NO_PUNCT=1 to skip punctuation pass.
"""

import argparse
import asyncio
import json
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

import websockets


RECORDINGS_DIR = Path(os.environ.get("RECORDINGS_DIR", "/opt/recorder/recordings"))
ASR_WS_URL     = os.environ.get("ASR_WS_URL", "ws://127.0.0.1:6006")
PUNCT_BIN      = Path(os.environ.get(
    "ASR_PUNCT_BIN",
    "/opt/recorder/asr/tmp/sherpa-onnx-v1.13.0-linux-x64-shared/bin/sherpa-onnx-offline-punctuation"))
PUNCT_MODEL    = Path(os.environ.get(
    "ASR_PUNCT_MODEL",
    "/opt/recorder/asr/models/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx"))
SHERPA_LIB     = os.environ.get(
    "ASR_SHERPA_LIB",
    "/opt/recorder/asr/tmp/sherpa-onnx-v1.13.0-linux-x64-shared/lib")
NO_PUNCT       = os.environ.get("ASR_OFFLINE_NO_PUNCT") == "1"

SAMPLE_RATE   = 16000
CHUNK_SAMPLES = 1600     # 100ms @ 16k
CHUNK_BYTES   = CHUNK_SAMPLES * 4   # float32
# When pushing at full speed sherpa may not flush results until you slow
# down or signal "Done". To avoid OOM on long meetings, throttle modestly.
THROTTLE_PER_CHUNK = float(os.environ.get("ASR_OFFLINE_THROTTLE", "0.01"))
# Watchdog: if no ws message received for this long after sending Done,
# stop waiting (server likely done).
POST_DONE_TIMEOUT = 60.0
# Watchdog during streaming: if no message for 5 min, abort.
STREAM_SILENCE_TIMEOUT = 300.0


def log(*args):
    print(time.strftime("%H:%M:%S"), *args, flush=True)


def open_ffmpeg(mp4_path):
    """Spawn ffmpeg decoding mp4 → 16k mono float32 PCM on stdout.

    Full-speed (no -re) — we want to process faster than real-time.
    """
    return subprocess.Popen(
        [
            "ffmpeg",
            "-loglevel", "error",
            "-i", str(mp4_path),
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


def punctuate_sync(text):
    """One subprocess invocation of sherpa-onnx-offline-punctuation. ~0.5s.

    Returns punctuated text or None on any failure (caller falls back to
    raw).
    """
    if NO_PUNCT or not PUNCT_BIN.is_file() or not PUNCT_MODEL.is_file():
        return None
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = SHERPA_LIB + ":" + env.get("LD_LIBRARY_PATH", "")
    try:
        r = subprocess.run(
            [str(PUNCT_BIN),
             f"--ct-transformer={PUNCT_MODEL}",
             "--num-threads=2",
             text],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=env,
            timeout=15,
        )
    except Exception:
        return None
    if r.returncode != 0:
        return None
    lines = r.stdout.decode("utf-8", "replace").splitlines()
    for i, ln in enumerate(lines):
        if "Input text:" in ln and i + 1 < len(lines):
            return lines[i + 1].strip()
    return None


async def feeder(ws, ff_stdout):
    """ffmpeg PCM → sherpa websocket. Returns total samples sent."""
    loop = asyncio.get_event_loop()
    samples_total = 0
    chunks = 0
    while True:
        raw = await loop.run_in_executor(None, ff_stdout.read, CHUNK_BYTES)
        if not raw or len(raw) < 4:
            return samples_total
        n = len(raw) // 4
        await ws.send(struct.pack("<i", n) + raw[: n * 4])
        samples_total += n
        chunks += 1
        # Progress every 5000 chunks (~ 500 seconds of audio)
        if chunks % 5000 == 0:
            log(f"  feeder: pushed {samples_total / SAMPLE_RATE:.0f}s of audio")
        if THROTTLE_PER_CHUNK > 0:
            await asyncio.sleep(THROTTLE_PER_CHUNK)


async def receiver(ws, finals_out, t0_unix, state):
    """Consume sherpa text messages. Append finals to finals_out."""
    while True:
        try:
            msg = await ws.recv()
        except websockets.ConnectionClosed:
            return
        state["last_recv_ts"] = time.time()
        if isinstance(msg, bytes):
            continue
        try:
            d = json.loads(msg)
        except Exception:
            continue
        text = (d.get("text") or "").strip()
        if not text:
            continue
        if not d.get("is_final", False):
            continue
        seg_start = d.get("start_time") or 0
        finals_out.append({
            "t":          t0_unix + float(seg_start),
            "text":       text,
            "is_final":   True,
            "segment":    int(d.get("segment", len(finals_out))),
            "tokens":     d.get("tokens") or [],
            "timestamps": d.get("timestamps") or [],
            "start_time": float(seg_start),
        })
        log(f"  [seg {len(finals_out)-1:>3}] {text[:80]}{'…' if len(text)>80 else ''}")


async def watchdog(state, stream_done):
    """Abort if no ws message arrives for too long while streaming."""
    while not stream_done.is_set():
        await asyncio.sleep(10.0)
        idle = time.time() - state["last_recv_ts"]
        if idle > STREAM_SILENCE_TIMEOUT:
            log(f"WATCHDOG: {idle:.0f}s no ws message — aborting")
            stream_done.set()
            return


async def process_meeting(meeting_id, mp4_path, t0_unix):
    log(f"connecting sherpa @ {ASR_WS_URL}")
    finals = []
    state = {"last_recv_ts": time.time()}
    stream_done = asyncio.Event()

    async with websockets.connect(ASR_WS_URL, max_size=10_000_000,
                                   open_timeout=10) as ws:
        log("connected; spawning ffmpeg")
        ff = open_ffmpeg(mp4_path)
        feed_task = asyncio.create_task(feeder(ws, ff.stdout))
        recv_task = asyncio.create_task(receiver(ws, finals, t0_unix, state))
        wd_task   = asyncio.create_task(watchdog(state, stream_done))

        try:
            total_samples = await feed_task
        except Exception as e:
            log(f"feeder error: {e!r}")
            total_samples = 0
        stream_done.set()
        log(f"all audio pushed: {total_samples / SAMPLE_RATE:.1f}s; signalling Done")
        try:
            await ws.send("Done")
        except Exception:
            pass

        # 等 server 处理 tail audio。POST_DONE_TIMEOUT 内没新 message 就退。
        last_count = len(finals)
        idle_start = time.time()
        while time.time() - idle_start < POST_DONE_TIMEOUT:
            await asyncio.sleep(2.0)
            if len(finals) > last_count:
                last_count = len(finals)
                idle_start = time.time()
            else:
                # 检查 last recv 时间
                if time.time() - state["last_recv_ts"] > 5.0:
                    # 5 秒没新消息 + 已经 Done — 可以退
                    if time.time() - idle_start > 10.0:
                        break
        recv_task.cancel()
        wd_task.cancel()

        try:
            ff.terminate()
            ff.wait(timeout=2)
        except Exception:
            try: ff.kill()
            except Exception: pass

    return finals


def write_transcript_jsonl(out_path, finals, meeting_id, append=False):
    """Write each final + its punct version (if any) as two JSONL lines.

    Matches the format that recorder-asr-bridge writes for the live path:
    raw first, then punct=true (with replaces_segment=<seg>) for the
    transcript.json dedup logic to pick the punct version.

    If `append` is True, opens the file in append mode — useful for
    rescuing a meeting that was recorded in multiple mp4 segments
    (main_01..main_NN), invoke once per segment.
    """
    log(f"running punctuation on {len(finals)} finals...")
    punct_results = []
    for i, d in enumerate(finals):
        pt = punctuate_sync(d["text"])
        punct_results.append(pt if (pt and pt != d["text"]) else None)
        if (i + 1) % 20 == 0:
            log(f"  punct progress: {i + 1}/{len(finals)}")

    mode = "a" if append else "w"
    log(f"{'appending to' if append else 'writing'} {out_path}")
    with out_path.open(mode, encoding="utf-8") as f:
        for i, d in enumerate(finals):
            f.write(json.dumps({
                "t":          d["t"],
                "text":       d["text"],
                "is_final":   True,
                "segment":    d["segment"],
                "tokens":     d["tokens"],
                "timestamps": d["timestamps"],
                "start_time": d["start_time"],
                "meeting_id": meeting_id,
            }, ensure_ascii=False) + "\n")
            if punct_results[i]:
                f.write(json.dumps({
                    "t":                 d["t"] + 0.001,
                    "text":              punct_results[i],
                    "is_final":          True,
                    "punct":             True,
                    "segment":           d["segment"],
                    "replaces_segment":  d["segment"],
                    "meeting_id":        meeting_id,
                }, ensure_ascii=False) + "\n")

    n_punct = sum(1 for p in punct_results if p)
    log(f"done. {len(finals)} finals ({n_punct} also have punct version)")


def main():
    ap = argparse.ArgumentParser(description="Offline ASR for an existing meeting mp4")
    ap.add_argument("meeting_id",       help="e.g. 20260513_820686")
    ap.add_argument("--mp4",            default="",
                    help="mp4 filename within meeting dir (default: main_01.mp4 or first main_*.mp4)")
    ap.add_argument("--overwrite",      action="store_true",
                    help="overwrite existing transcript.jsonl")
    ap.add_argument("--append",         action="store_true",
                    help="append to existing transcript.jsonl (use for multi-mp4 meeting rescue)")
    args = ap.parse_args()

    meeting_dir = RECORDINGS_DIR / args.meeting_id
    if not meeting_dir.is_dir():
        print(f"ERROR: meeting dir not found: {meeting_dir}", file=sys.stderr)
        return 1

    if args.mp4:
        mp4_path = meeting_dir / args.mp4
    else:
        mp4_path = meeting_dir / "main_01.mp4"
        if not mp4_path.is_file():
            candidates = sorted(meeting_dir.glob("main_*.mp4"))
            if not candidates:
                print(f"ERROR: no main_*.mp4 in {meeting_dir}", file=sys.stderr)
                return 1
            mp4_path = candidates[0]
    if not mp4_path.is_file():
        print(f"ERROR: mp4 not found: {mp4_path}", file=sys.stderr)
        return 1

    out_path = meeting_dir / "transcript.jsonl"
    if out_path.exists() and not args.overwrite and not args.append:
        print(f"ERROR: {out_path} already exists. use --overwrite or --append.",
              file=sys.stderr)
        return 1
    if args.overwrite and args.append:
        print("ERROR: --overwrite and --append are mutually exclusive", file=sys.stderr)
        return 1

    # 读 meeting.json 拿对应 mp4 的 wall_start_ms 作 absolute t 基准
    t0_unix = 0.0
    mj_path = meeting_dir / "meeting.json"
    if mj_path.exists():
        try:
            mj = json.loads(mj_path.read_text(encoding="utf-8"))
            for s in mj.get("segments", []):
                if s.get("file") == mp4_path.name and s.get("wall_start_ms"):
                    t0_unix = s["wall_start_ms"] / 1000.0
                    break
            if t0_unix == 0.0 and mj.get("start_wall_ms"):
                t0_unix = mj["start_wall_ms"] / 1000.0
        except Exception:
            pass

    log(f"meeting={args.meeting_id}  mp4={mp4_path.name}  t0={t0_unix:.3f}")
    log(f"will write: {out_path}")

    started = time.time()
    finals = asyncio.run(process_meeting(args.meeting_id, mp4_path, t0_unix))
    elapsed = time.time() - started
    log(f"ASR done in {elapsed:.0f}s, got {len(finals)} finals")
    if not finals:
        log("no transcripts received — NOT writing empty file")
        return 1

    write_transcript_jsonl(out_path, finals, args.meeting_id, append=args.append)
    log("SUCCESS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
