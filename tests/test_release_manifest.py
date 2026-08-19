from __future__ import annotations

import json
import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "make_release_manifest.py"


def run_manifest(build: pathlib.Path, *extra: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), "--build", str(build),
         "--profile", "MITM_NTS", "--transport", "MQTT", *extra],
        cwd=ROOT,
        text=True,
        capture_output=True,
        check=False,
    )


def test_hil_pass_requires_evidence(tmp_path: pathlib.Path) -> None:
    result = run_manifest(tmp_path, "--physical-hil", "PASS")
    assert result.returncode != 0
    assert "--hil-evidence is required" in result.stderr


def test_hil_pass_and_evidence_are_recorded(tmp_path: pathlib.Path) -> None:
    firmware = tmp_path / "klima_wifi.bin"
    firmware.write_bytes(b"exact-artifact")
    evidence = "docs/VALIDATION.md"
    result = run_manifest(
        tmp_path,
        "--physical-hil", "PASS",
        "--hil-evidence", evidence,
    )
    assert result.returncode == 0, result.stderr
    manifest = json.loads((tmp_path / "release-manifest.json").read_text("utf-8"))
    assert manifest["physical_hil"] == "PASS"
    assert manifest["hil_evidence"] == evidence
    assert manifest["artifacts"][0]["path"] == "klima_wifi.bin"


def test_default_manifest_remains_not_run(tmp_path: pathlib.Path) -> None:
    (tmp_path / "klima_wifi.bin").write_bytes(b"build-only")
    result = run_manifest(tmp_path)
    assert result.returncode == 0, result.stderr
    manifest = json.loads((tmp_path / "release-manifest.json").read_text("utf-8"))
    assert manifest["physical_hil"] == "NOT_RUN"
    assert manifest["hil_evidence"] is None
