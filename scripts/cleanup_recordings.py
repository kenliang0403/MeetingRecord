#!/usr/bin/env python3
"""录像保留策略清理脚本。

由 recorder-cleanup.timer 每天调用一次。读 /opt/recorder/config/config.json
的 cleanup 节决定行为：

    "cleanup": { "enabled": false, "retention_days": 180 }

删除规则（安全优先）：
  - 只删 recorder.output_dir 下、**目录名以 YYYYMMDD 开头**的会议目录
    （如 20260526_820715）。不符合该前缀的目录一律不动。
  - 按目录名里的日期判断是否超期，**不用 mtime**（mtime 会被 faststart
    后处理、rsync 等操作刷新，导致误判）。
  - 跳过 recorder-core 当前正在录制的会议（查 TCP 9001 status）。
  - enabled=false 时不删（除非 --force 或 --dry-run）。

用法：
    python3 cleanup_recordings.py                # 正常执行（受 enabled 控制）
    python3 cleanup_recordings.py --dry-run      # 只预览，不删
    python3 cleanup_recordings.py --force        # 忽略 enabled=false 跑一次
    python3 cleanup_recordings.py --config /path/to/config.json

只用标准库，不需要 venv。
"""
import argparse
import json
import os
import re
import shutil
import socket
import sys
from datetime import datetime, timedelta
from pathlib import Path

# 会议目录名以 8 位日期开头：20260526_820715 / 20260526 等
DIR_DATE_RE = re.compile(r'^(\d{8})')


def load_cfg(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except Exception as e:
        print(f"[cleanup] 读取配置失败 {path}: {e}", file=sys.stderr)
        return {}


def current_meeting_id(host="127.0.0.1", port=9001, timeout=3):
    """问 recorder-core 当前正在录制的 meeting_id（取不到返回空串）。"""
    try:
        s = socket.socket()
        s.settimeout(timeout)
        s.connect((host, port))
        s.sendall(b'{"cmd":"status"}\n')   # 必须发 JSON（见 docs/api.md §2）
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(8192)
            if not chunk:
                break
            buf += chunk
        s.close()
        data = json.loads(buf.decode())
        return (data.get("data") or {}).get("meeting_id") or ""
    except Exception:
        return ""   # recorder-core 不可达：不阻塞清理历史会议


def main():
    ap = argparse.ArgumentParser(description="录像保留策略清理")
    ap.add_argument("--config",
                    default=os.environ.get("RECORDER_CONFIG",
                                           "/opt/recorder/config/config.json"))
    ap.add_argument("--dry-run", action="store_true",
                    help="只列出将删除的目录，不真正删")
    ap.add_argument("--force", action="store_true",
                    help="忽略 cleanup.enabled=false，强制执行一次")
    args = ap.parse_args()

    cfg = load_cfg(args.config)
    cu = cfg.get("cleanup") or {}
    enabled = bool(cu.get("enabled", False))
    retention = int(cu.get("retention_days", 180))
    rec_dir = ((cfg.get("recorder") or {}).get("output_dir")
               or "/opt/recorder/recordings")

    if not enabled and not args.force and not args.dry_run:
        print("[cleanup] cleanup.enabled=false，跳过（--force 强制 / --dry-run 预览）")
        return 0
    if retention <= 0:
        print(f"[cleanup] retention_days={retention} 非法（必须 >0），中止",
              file=sys.stderr)
        return 1

    base = Path(rec_dir)
    if not base.is_dir():
        print(f"[cleanup] 录像目录不存在: {base}", file=sys.stderr)
        return 1

    cutoff = datetime.now() - timedelta(days=retention)
    cur_meeting = current_meeting_id()
    mode = "DRY-RUN 预览" if args.dry_run else "执行删除"
    print(f"[cleanup] {mode}  目录={base}  保留={retention}天  "
          f"截止日期<{cutoff.date()}"
          + (f"  当前在录={cur_meeting}（跳过）" if cur_meeting else ""))

    deleted, freed = 0, 0
    for d in sorted(base.iterdir()):
        if not d.is_dir():
            continue
        m = DIR_DATE_RE.match(d.name)
        if not m:
            continue   # 非 YYYYMMDD 前缀目录：安全起见不动
        try:
            dt = datetime.strptime(m.group(1), "%Y%m%d")
        except ValueError:
            continue
        if dt >= cutoff:
            continue   # 未超期
        if cur_meeting and d.name == cur_meeting:
            print(f"[cleanup] 跳过正在录制: {d.name}")
            continue
        try:
            size = sum(f.stat().st_size for f in d.rglob('*') if f.is_file())
        except Exception:
            size = 0
        if args.dry_run:
            print(f"[cleanup] [DRY] 将删除 {d.name}  "
                  f"({size / 1048576:.1f} MB, 日期 {dt.date()})")
        else:
            try:
                shutil.rmtree(d)
                deleted += 1
                freed += size
                print(f"[cleanup] 已删除 {d.name}  "
                      f"({size / 1048576:.1f} MB, 日期 {dt.date()})")
            except Exception as e:
                print(f"[cleanup] 删除失败 {d.name}: {e}", file=sys.stderr)

    if not args.dry_run:
        print(f"[cleanup] 完成：删除 {deleted} 个会议，释放 "
              f"{freed / 1073741824:.2f} GB（保留 {retention} 天）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
