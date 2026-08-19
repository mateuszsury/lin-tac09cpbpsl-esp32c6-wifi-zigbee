from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]


def test_unconfigured_http_control_is_fail_closed():
    secrets = (ROOT / "main" / "klima_secrets.h").read_text(encoding="utf-8")
    web = (ROOT / "main" / "web_service.c").read_text(encoding="utf-8")
    assert '#define KLIMA_DEVICE_TOKEN ""' in secrets
    assert "expected_length < 24U" in web


def test_fallback_ap_requires_an_explicit_strong_password():
    wifi = (ROOT / "main" / "wifi_service.c").read_text(encoding="utf-8")
    assert "ap_password_length >= 12U" in wifi
    assert "ap_enabled ? WIFI_MODE_APSTA : WIFI_MODE_STA" in wifi
    assert "ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK" in wifi


def test_public_tree_does_not_contain_known_private_defaults():
    forbidden = (
        "development-token" + "-must-be-replaced",
        "replace-with-a-" + "real-password",
    )
    private_patterns = (
        re.compile(r"https?://192\.168\.(?!4\.1(?:[:/]|$))\d{1,3}\.\d{1,3}"),
        re.compile(r"[A-Za-z]:\\Users\\[^<\\/]+\\"),
        re.compile(r"/home/(?!<user>/)[^/\s]+/"),
    )
    public_files = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
        cwd=ROOT,
        text=True,
    ).splitlines()
    for relative in public_files:
        path = ROOT / relative
        if path.resolve() == Path(__file__).resolve():
            continue
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for value in forbidden:
            assert value not in text, f"private marker {value!r} in {path}"
        for pattern in private_patterns:
            assert not pattern.search(text), f"private path or LAN URL in {path}"
