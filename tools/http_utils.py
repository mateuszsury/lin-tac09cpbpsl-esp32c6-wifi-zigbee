"""Strict HTTP URL handling shared by local Klima WiFi command-line tools."""

from __future__ import annotations

import urllib.request
from typing import Any
from urllib.parse import urlsplit


def validate_http_url(value: str) -> str:
    """Return an absolute HTTP(S) URL or reject local-file and credential URLs."""
    if not isinstance(value, str) or not value:
        raise ValueError("URL must be a non-empty string")
    if any(char.isspace() or ord(char) < 0x20 for char in value):
        raise ValueError("URL cannot contain whitespace or control characters")
    parsed = urlsplit(value)
    if parsed.scheme.lower() not in {"http", "https"}:
        raise ValueError("URL scheme must be http or https")
    if not parsed.hostname or "\\" in parsed.netloc:
        raise ValueError("URL must contain a valid network host")
    if parsed.username is not None or parsed.password is not None:
        raise ValueError("credentials are not allowed inside URLs")
    if parsed.fragment:
        raise ValueError("URL fragments are not allowed")
    try:
        parsed.port
    except ValueError as error:
        raise ValueError("URL contains an invalid port") from error
    return value


def normalize_http_base_url(value: str) -> str:
    """Validate a device/service origin and remove its optional trailing slash."""
    validated = validate_http_url(value)
    parsed = urlsplit(validated)
    if parsed.path not in {"", "/"} or parsed.query:
        raise ValueError("base URL must not contain a path, query, or fragment")
    return validated.rstrip("/")


def urlopen_http(target: str | urllib.request.Request, *, timeout: float) -> Any:
    """Open only an HTTP(S) request after validating its final URL."""
    url = target.full_url if isinstance(target, urllib.request.Request) else target
    validate_http_url(url)
    # The dynamic value has passed the scheme, host, credential and fragment
    # checks above. urllib is retained to keep these tools dependency-free.
    return urllib.request.urlopen(  # nosemgrep: python.lang.security.audit.dynamic-urllib-use-detected.dynamic-urllib-use-detected
        target, timeout=timeout
    )
