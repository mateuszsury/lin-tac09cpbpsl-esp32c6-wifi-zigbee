"""Validate new maintainer-owned commit identities without rejecting contributors."""
import os
from pathlib import Path
import re
import subprocess

CANONICAL_NAME = 'Mateusz Sury'
CANONICAL_EMAIL = '66475105+mateuszsury@users.noreply.github.com'
OWN_EMAILS = {CANONICAL_EMAIL, 'mateuszsury@users.noreply.github.com',
              'mateusz.sury25@gmail.com', 'mateuszsury25@gmail.com',
              'klima-wifi@users.noreply.github.com'}

def git(*args):
    return subprocess.check_output(['git', *args], text=True).strip()

def main():
    head = os.environ.get('HEAD_SHA') or 'HEAD'
    base = os.environ.get('BASE_SHA', '')
    # A new branch has no 'before' revision; its PR validates the complete range.
    if base and set(base) != {'0'}:
        known = subprocess.run(['git', 'cat-file', '-e', base + '^{commit}'], capture_output=True).returncode == 0
        # After an intentional history migration, old revisions need not be in
        # a fresh checkout. Validate the replacement history rather than fail.
        revision_range = base + '..' + head if known else head
        if not known and os.environ.get('DEFAULT_BRANCH'):
            default_ref = 'refs/remotes/origin/' + os.environ['DEFAULT_BRANCH']
            ancestor = subprocess.run(['git', 'merge-base', head, default_ref], capture_output=True, text=True)
            if ancestor.returncode == 0 and git('rev-parse', default_ref) != git('rev-parse', head):
                # An amended feature branch may lose its old before object.
                # Check its new changes against default, without rechecking
                # protected pre-policy history. Replaced default histories
                # still validate in full because default_ref equals head.
                revision_range = ancestor.stdout.strip() + '..' + head
        commits = git('rev-list', revision_range).splitlines()
    else:
        commits = [git('rev-parse', head)]
    failures = []
    for sha in commits:
        name, email, message = git('show', '-s', '--format=%an%x00%ae%x00%B', sha).split('\0', 2)
        owned = email in OWN_EMAILS or email.lower() == 'noreply@anthropic.com'
        if owned and (name != CANONICAL_NAME or email != CANONICAL_EMAIL):
            failures.append(sha[:12] + ': use the configured maintainer Git identity')
        if owned and re.search(r'^(?:Co-authored-by:.*noreply@anthropic\.com|Claude-Session:)', message, re.I | re.M):
            failures.append(sha[:12] + ': remove AI attribution from maintainer-owned changes')
    if failures:
        raise SystemExit('\n'.join(failures))
    print(f'Author policy: {len(commits)} new commit(s) checked; real contributors preserved')

if __name__ == '__main__':
    main()
