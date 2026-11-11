# Build from the public source

Use Windows x64, Git, Python 3.12 or later, at least 8 GB free RAM and ample free
disk space. Run commands in the extracted `prototype` folder. All paths may
contain spaces; quote the ISO argument. No parent `research` folder is needed.
Dependencies are installed only inside `toolchains/`.

## Prepare shared inputs

```powershell
python tools/setup_windows.py
python tools/build_tool.py
python tools/prepare_release_inputs.py --iso "D:/Games/your-original.iso"
```

`setup_windows.py` checks the exact w64devkit 2.11.0 and FFmpeg 7.1.4 archive
hashes in `metadata/dependencies.json` before extraction. `--offline` uses cached
archives without downloading. Keep the official archives in `toolchains/` for
repeat builds. `build_tool.py` checks the donor commit in a Git checkout, or every
donor source hash in the exported source manifest, then compiles the recompiler
and runs its synthetic runtime tests.

Preparation accepts only the original ULES00719 Europe ISO, 884,146,176 bytes,
SHA-256 `64ab1ba788baade948b9b89d47145a653c063e85afed403b89d917d8a8703ec4`.
It verifies BOOT, relocates the PRX at `0x08804000`, checks the exact resulting
image hash, reads the 511 constructor seeds, regenerates C with strict executable
ranges, checks the display-hook source hashes, generates import guards, and
splits registration while preserving every call and its order.

The private outputs are `work/input/`, `work/generated-ctors/`,
`build/asphalt_import_catalog.h`, `build/asphalt_missing_guards.h` and
`build/registration-split/`. They are never public source inputs. A local
`build/release-inputs.json` records the selected ISO path and hashes. The ISO is
read only; it is not moved, copied or modified.

## Windows x64

```powershell
python tools/build_ffmpeg_windows.py
python tools/build_windows.py
python tools/package_windows.py
```

See [Windows build and play](windows.md) for the output ZIP and controls. Players
select their supported ISO on first run; they need neither Python nor a compiler.
The Windows build uses separate object and media-library directories from Android.

## Android ARM64

```powershell
python tools/setup_android.py
python tools/build_ffmpeg_android.py
python tools/build_android_native.py --hud-diagnostics
python tools/build_apk.py --iso "D:/Games/your-original.iso"
```

The existing Android commands and application sources are retained. The Android
setup pins NDK r28c, Android API/build tools 35 and Microsoft JDK 21.0.13. The
precise archive hashes are also recorded in `metadata/dependencies.json`.
The APK command's original parent-folder ISO filename remains its default;
`--iso` allows any readable path to that exact supported original.

The APK includes the locally generated startup image and shared native library,
and reads remaining game content from the user's selected ISO. Its build uses a
new local test signing key if one is absent. Keep that key private and retain it
for updates to your own installation. No existing key is included in source.
The exported builder hashes that local key's DER certificate and requires the
APK signature verifier to report the same certificate; your key is independent
of the private project's previous releases.

The source export's neutral icon has its matching exact SHA-256 guard in the
exported APK script. The four supplied touch-control PNGs retain their guards.

## Source checks and export

```powershell
python tools/test_release_source.py
python tools/test_prx_image.py
python tools/check_public_source.py
python tools/export_source.py
```

The default archive is `build/releases/Asphalt-Urban-GT-2-v1.0-source.zip`.
The exporter uses an allowlist and fixed ZIP timestamps. It replaces the private
README and application artwork with public versions, includes both hosts and the
patched MIT donor, and omits game inputs/generated C, toolchain binaries,
research, captures, saves, logs and signing keys. Public source manifests record
every included file hash. GitHub Actions runs these source checks without a game
upload; full game builds are local because they require the user's original ISO.

Exact compiler/library versions and build settings are pinned, but host paths,
timestamps and local signing keys can affect binary bytes. The source ZIP itself
is byte-for-byte deterministic for the same input source tree.
