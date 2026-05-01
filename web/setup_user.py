#!/usr/bin/env python3
"""Set or reset a user's password in /opt/recorder/web/auth.json.

Usage:
    sudo python3 setup_user.py <username>
    # Will prompt twice for password.

The plaintext password is never stored. Only salt + sha256(salt+pwd).
"""
import getpass
import os
import sys

# Make this script runnable from anywhere by adding its dir to path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from auth import AUTH_FILE, load_users, save_users, hash_password


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <username>", file=sys.stderr)
        return 2

    username = sys.argv[1]
    pwd1 = getpass.getpass(f"new password for '{username}': ")
    pwd2 = getpass.getpass("confirm: ")
    if pwd1 != pwd2:
        print("passwords do not match", file=sys.stderr)
        return 1
    if len(pwd1) < 6:
        print("password too short (min 6 chars)", file=sys.stderr)
        return 1

    salt, h = hash_password(pwd1)
    data = load_users()
    data.setdefault("users", {})[username] = {"salt": salt, "hash": h}
    os.makedirs(os.path.dirname(AUTH_FILE), exist_ok=True)
    save_users(data)
    print(f"OK: {username} written to {AUTH_FILE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
