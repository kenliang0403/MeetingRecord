"""Simple file-based auth: salted SHA-256 password storage + Flask sessions.

We deliberately avoid a database. Users live in a single JSON file
(default `/opt/recorder/web/auth.json`) with mode 0600, owned by the
service user. `setup_user.py` is a CLI to create/reset users.

The browser only ever sees a session cookie (signed via Flask SECRET_KEY).
The plaintext password never reaches the JS side.

`from __future__ import annotations` lets us use 3.9+ generic syntax
(`tuple[str, str]`) on the deployment target's Python 3.7.
"""
from __future__ import annotations
import hashlib
import json
import os
import secrets
from functools import wraps
from typing import Optional

from flask import session, redirect, url_for, request, flash


# --- Default location -------------------------------------------------

AUTH_FILE = os.environ.get("RECORDER_AUTH_FILE", "/opt/recorder/web/auth.json")


# --- Hash helpers -----------------------------------------------------

def hash_password(password: str, salt: Optional[str] = None) -> tuple[str, str]:
    """Return (salt_hex, hash_hex). Salt is 16 random bytes if not provided."""
    if salt is None:
        salt = secrets.token_hex(16)
    h = hashlib.sha256()
    h.update(bytes.fromhex(salt))
    h.update(password.encode("utf-8"))
    return salt, h.hexdigest()


def verify_password(password: str, salt: str, expected_hash: str) -> bool:
    _, computed = hash_password(password, salt)
    # constant-time compare
    return secrets.compare_digest(computed, expected_hash)


# --- File I/O ---------------------------------------------------------

def load_users(path: str = AUTH_FILE) -> dict:
    if not os.path.exists(path):
        return {"users": {}}
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def save_users(data: dict, path: str = AUTH_FILE) -> None:
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    os.replace(tmp, path)
    try:
        os.chmod(path, 0o600)
    except OSError:
        pass


def authenticate(username: str, password: str, path: str = AUTH_FILE) -> bool:
    data = load_users(path)
    user = data.get("users", {}).get(username)
    if not user:
        return False
    return verify_password(password, user["salt"], user["hash"])


# --- Flask session decorator ------------------------------------------

def login_required(view_func):
    @wraps(view_func)
    def wrapper(*args, **kwargs):
        if not session.get("user"):
            # remember where they wanted to go
            return redirect(url_for("login", next=request.path))
        return view_func(*args, **kwargs)
    return wrapper
