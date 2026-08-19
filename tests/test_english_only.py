from __future__ import annotations

import pathlib
import re
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[1]

POLISH_DIACRITICS = re.compile(
    "[\u0105\u0107\u0119\u0142\u0144\u00f3\u015b\u017a\u017c"
    "\u0104\u0106\u0118\u0141\u0143\u00d3\u015a\u0179\u017b]"
)
POLISH_WORDS = re.compile(
    r"\b(?:klimatyzator|urzadzenie|sterowanie|ustawienia|polaczenie|"
    r"odlaczone|odlaczony|odlaczone|wpisz|wybierz|wgrano|poczekaj|"
    r"przerwano|zakonczyl|oczekiwano|nieprawidlowy|obraz)\b",
    re.IGNORECASE,
)


def tracked_text_files() -> list[pathlib.Path]:
    result = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=ROOT,
        capture_output=True,
        check=True,
    )
    paths: list[pathlib.Path] = []
    for raw in result.stdout.split(b"\0"):
        if not raw:
            continue
        path = ROOT / raw.decode("utf-8")
        try:
            path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        paths.append(path)
    return paths


def test_tracked_text_is_english_only() -> None:
    failures: list[str] = []
    this_file = pathlib.Path(__file__).resolve()
    for path in tracked_text_files():
        if path.resolve() == this_file:
            continue
        text = path.read_text(encoding="utf-8")
        for line_number, line in enumerate(text.splitlines(), start=1):
            if POLISH_DIACRITICS.search(line) or POLISH_WORDS.search(line):
                failures.append(f"{path.relative_to(ROOT)}:{line_number}: {line.strip()}")
    assert not failures, "Non-English repository text found:\n" + "\n".join(failures)
