# Publishing on GitHub

## Pre-publication checklist

1. Run `git status --short` and review every file.
2. Confirm local configuration lives only in ignored
   `main/klima_secrets_local.h` and `.local/`.
3. Search both the working tree and history for credentials, private IPs,
   personal paths and dashboard exports.
4. Run tests and the complete build matrix.
5. Generate fresh manifests for binaries you intend to attach to a release.
6. Confirm `LICENSE`, `SECURITY.md`, contribution rules and issue templates are
   present.
7. Push to a new private GitHub repository first, enable secret scanning, then
   switch visibility to public only after reviewing GitHub's file list.

Never upload `.local`, `build`, `managed_components`, `sdkconfig`, complete Home
Assistant exports, screenshots containing private entity names, raw captures or
firmware built with embedded credentials.

## Create the remote

Using GitHub CLI after creating an empty repository:

```powershell
gh auth status
git remote add origin https://github.com/<owner>/<repository>.git
git push -u origin main
```

Or let `gh` create it as private first:

```powershell
gh repo create <owner>/<repository> --private --source . --remote origin --push
```

Review **Settings -> Security -> Code security** and enable secret scanning,
push protection and Dependabot alerts where available. Add branch protection
requiring the CI jobs before changing visibility.

## First release

1. Build from the exact tagged commit.
2. Verify each manifest and SHA-256.
3. Attach only application binaries whose credential policy is appropriate for
   distribution. In most cases, publish source and require users to build their
   own credentialed image.
4. State hardware compatibility and every `NOT_RUN` gate in release notes.
5. Tag using semantic versioning, for example:

```powershell
git tag -a v0.9.39 -m "Klima WiFi v0.9.39"
git push origin v0.9.39
gh release create v0.9.39 --generate-notes
```

Do not advertise Zigbee as production-ready until a real Zigbee2MQTT
join/interview/reporting/command/OTA cycle passes.

## If a secret ever entered history

Rotate it first. Deleting the current file is insufficient because old commits
remain downloadable. Rewrite or replace the affected history, force-push only
after coordinating with contributors, invalidate cached artifacts and verify
the entire remote again. Treat Wi-Fi, MQTT, Home Assistant tokens and device
tokens as compromised once pushed.
