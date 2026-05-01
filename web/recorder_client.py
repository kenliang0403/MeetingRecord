"""TCP client for recorder-core ControlServer (port 9001).

ControlServer accepts JSON-line requests and returns one JSON-line response.
This module wraps that with a small connection-per-call helper, since
ControlServer doesn't keep state across requests.
"""
import json
import socket
from typing import Any


class RecorderClient:
    def __init__(self, host: str = "127.0.0.1", port: int = 9001, timeout: float = 3.0):
        self.host = host
        self.port = port
        self.timeout = timeout

    def call(self, cmd: str, **kwargs: Any) -> dict:
        req = {"cmd": cmd, **kwargs}
        try:
            with socket.create_connection((self.host, self.port), self.timeout) as s:
                s.sendall((json.dumps(req) + "\n").encode())
                buf = bytearray()
                s.settimeout(self.timeout)
                while True:
                    chunk = s.recv(8192)
                    if not chunk:
                        break
                    buf += chunk
                    if b"\n" in chunk:
                        break
            text = buf.decode("utf-8", errors="replace").strip()
            return json.loads(text) if text else {"ok": False, "error": "empty response"}
        except (OSError, socket.timeout) as e:
            return {"ok": False, "error": f"recorder-core unreachable: {e}"}
        except json.JSONDecodeError as e:
            return {"ok": False, "error": f"bad response: {e}"}

    # Convenience wrappers ------------------------------------------------

    def status(self) -> dict:
        return self.call("status")

    def config(self) -> dict:
        return self.call("config")

    def audio_levels(self) -> dict:
        return self.call("audio_levels")

    def recordings(self) -> dict:
        return self.call("recordings")
