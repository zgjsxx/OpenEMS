"""Authentication helpers for the OpenEMS web console."""

from __future__ import annotations

import hashlib
import hmac
import secrets
from typing import Tuple


SESSION_COOKIE = "openems_session"


def hash_password(password: str, salt: str | None = None, rounds: int = 180000) -> str:
    salt = salt or secrets.token_hex(16)
    digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt.encode("utf-8"), rounds).hex()
    return f"pbkdf2_sha256${salt}${rounds}${digest}"


def verify_password(password: str, password_hash: str) -> bool:
    try:
        scheme, salt, rounds_text, digest = password_hash.split("$", 3)
        if scheme != "pbkdf2_sha256":
            return False
        rounds = int(rounds_text)
    except ValueError:
        return False

    candidate = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt.encode("utf-8"), rounds).hex()
    return hmac.compare_digest(candidate, digest)


def create_session_token() -> Tuple[str, str]:
    raw_token = secrets.token_urlsafe(32)
    session_id = secrets.token_hex(16)
    return session_id, f"{session_id}.{raw_token}"


def split_session_token(cookie_value: str) -> Tuple[str, str]:
    if "." not in cookie_value:
        raise ValueError("Invalid session cookie format.")
    return cookie_value.split(".", 1)


def hash_session_secret(secret: str) -> str:
    return hashlib.sha256(secret.encode("utf-8")).hexdigest()
