# Asphalt Urban GT 2 — Windows and Android

A native recompilation project for Asphalt Urban GT 2 (PSP, Europe ULES00719).
Windows and Android use the same generated game code, patched PSP runtime and
handwritten host extensions. Windows runs as a portable x64 application; Android
builds an ARM64 APK.

You must provide your own supported original ISO. This source contains no game
executable, generated game C, disc data, emulator or firmware. Gameplay preview
images are included in `screenshots/`.
The public source uses an original neutral application icon.

Read [build instructions](docs/building.md) for dependency setup and both targets,
and [Windows instructions](docs/windows.md) for controls and portable packaging.

The only supported ISO has SHA-256:

```text
64ab1ba788baade948b9b89d47145a653c063e85afed403b89d917d8a8703ec4
```

GitHub Actions checks source structure and synthetic tests. It does not download
or accept a secret copy of the game. Build game outputs locally with your ISO.
Do not commit generated work/, build/, toolchains/, signing keys or saves.

## Screenshots

| | |
|---|---|
| ![Daytime race with police traffic](screenshots/Screenshot_20261110_013528.jpg) | ![Night race with a classic roadster](screenshots/Screenshot_20261110_012830.jpg) |
| ![Nitro drift on wet city streets](screenshots/Screenshot_20261110_162103.jpg) | ![Night race with nitro](screenshots/Screenshot_20261110_012944.jpg) |

## Source and dependencies

The patched donor is pinned at commit
`0ad357fed8fd4a59226034ff6e61aaba2a0d27bb`; its complete included source is
recorded in `vendor/psprecomp/SOURCE_MANIFEST.json`. MIT notices and FFmpeg LGPL
requirements are described in [source licensing](docs/source-license.md) and
`THIRD_PARTY_LICENSES.txt`. Game rights remain with their owners.

This archive is prepared for local review and manual publication. Its creation
does not create a repository, upload files or publish a release.
