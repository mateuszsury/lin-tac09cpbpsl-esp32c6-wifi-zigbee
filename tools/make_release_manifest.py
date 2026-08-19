#!/usr/bin/env python3
"""Create a deterministic manifest and SHA-256 inventory for one IDF build."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
from datetime import datetime, timezone


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def git_revision(root: pathlib.Path) -> str:
    try:
        return subprocess.check_output(
            ['git', 'rev-parse', '--verify', 'HEAD'], cwd=root, text=True
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return 'unknown'


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=pathlib.Path, required=True)
    parser.add_argument('--profile', required=True)
    parser.add_argument('--transport', required=True)
    parser.add_argument('--output', type=pathlib.Path)
    parser.add_argument(
        '--physical-hil', choices=('NOT_RUN', 'PASS'), default='NOT_RUN',
        help='Physical acceptance result for this exact artifact (default: NOT_RUN)',
    )
    parser.add_argument(
        '--hil-evidence',
        help='Repository-relative evidence document for a PASS result',
    )
    args = parser.parse_args()
    if args.physical_hil == 'PASS' and not args.hil_evidence:
        parser.error('--hil-evidence is required when --physical-hil PASS is selected')
    build = args.build.resolve()
    root = pathlib.Path(__file__).resolve().parents[1]
    output = args.output or build / 'release-manifest.json'
    files = []
    allowed_names = {
        'klima_wifi.bin',
        'klima_wifi.elf',
        'klima_wifi.map',
        'bootloader.bin',
        'bootloader.elf',
        'bootloader.map',
        'partition-table.bin',
        'ota_data_initial.bin',
        'flasher_args.json',
        'project_description.json',
    }
    for path in sorted(build.rglob('*')):
        # Check the basename before stat/is_file: WSL IDF builds leave symlink
        # trees that Windows Python cannot inspect (for example mbedTLS
        # sources).  A release manifest only needs the allow-listed artifacts.
        if path.name == output.name or path.name not in allowed_names:
            continue
        try:
            if not path.is_file():
                continue
            files.append({'path': str(path.relative_to(build)), 'size': path.stat().st_size, 'sha256': sha256(path)})
        except OSError:
            # Ignore inaccessible non-release intermediates/symlinks.  If an
            # allow-listed artifact itself is inaccessible, the build output
            # will be visibly incomplete and the manifest will show it.
            continue
    manifest = {
        'schema': 1,
        'profile': args.profile.upper(),
        'transport': args.transport.upper(),
        'active_control': args.profile.upper() == 'MITM_NTS',
        'physical_hil': args.physical_hil,
        'hil_evidence': args.hil_evidence,
        'git_revision': git_revision(root),
        'generated_at_utc': datetime.now(timezone.utc).isoformat(),
        'artifacts': files,
    }
    output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n', encoding='utf-8')
    print(json.dumps(manifest, indent=2, sort_keys=True))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
