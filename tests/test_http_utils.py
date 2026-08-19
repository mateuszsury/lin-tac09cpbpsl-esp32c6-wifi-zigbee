import importlib.util
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("http_utils", ROOT / "tools" / "http_utils.py")
assert SPEC and SPEC.loader
http_utils = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(http_utils)


@pytest.mark.parametrize(
    "value,expected",
    [
        ("http://klima-wifi.local", "http://klima-wifi.local"),
        ("https://example.test:8443/", "https://example.test:8443"),
        ("http://[::1]:8080", "http://[::1]:8080"),
    ],
)
def test_normalize_http_base_url_accepts_network_origins(value, expected):
    assert http_utils.normalize_http_base_url(value) == expected


@pytest.mark.parametrize(
    "value",
    [
        "file:///etc/passwd",
        "ftp://example.test/device",
        "http://user:password@example.test",
        "http://example.test/api",
        "http://example.test?x=1",
        "http://example.test/#fragment",
        "http://example.test\\@attacker.test",
        "http://example.test\nattacker.test",
        "http://example.test:99999",
        "not-a-url",
    ],
)
def test_normalize_http_base_url_rejects_non_origin_or_unsafe_values(value):
    with pytest.raises(ValueError):
        http_utils.normalize_http_base_url(value)


def test_validate_http_url_accepts_api_paths_and_queries():
    value = "http://klima-wifi.local/api/logs?after=10"
    assert http_utils.validate_http_url(value) == value
