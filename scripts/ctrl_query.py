#!/usr/bin/env python3
"""Simple TCP control client for recorder-core ControlServer."""
import socket
import sys
import json

HOST = "127.0.0.1"
PORT = 9001

def send(cmd_dict):
    s = socket.socket()
    s.settimeout(5)
    s.connect((HOST, PORT))
    s.sendall((json.dumps(cmd_dict) + "\n").encode())
    buf = b""
    try:
        while True:
            chunk = s.recv(8192)
            if not chunk:
                break
            buf += chunk
            if b"\n" in chunk:
                break
    except socket.timeout:
        pass
    s.close()
    return buf.decode(errors="replace")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: ctrl_query.py <cmd> [key=value ...]", file=sys.stderr)
        sys.exit(2)
    req = {"cmd": sys.argv[1]}
    for arg in sys.argv[2:]:
        if "=" in arg:
            k, v = arg.split("=", 1)
            req[k] = v
    print(send(req))
