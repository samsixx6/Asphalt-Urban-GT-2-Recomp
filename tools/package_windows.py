"""Package only the verified Windows player, shared codecs and their source/notices."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import tarfile
import zipfile

from build_ffmpeg_windows import ARCHIVE_SHA256, VERSION
from build_windows import audit_dependencies, sha

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/windows'
DEFAULT_OUTPUT = ROOT / 'build/releases/Asphalt-Urban-GT-2-v1.0-windows-x64.zip'

README = """Asphalt Urban GT 2 1.0 - Windows x64

Windows 10 version 1903+ or Windows 11 x64; OpenGL 3.3 driver required.
Extract this entire ZIP into a writable folder, then double-click Asphalt2.exe.
Keep the three FFmpeg DLLs beside the executable. No installation, Python,
compiler or emulator is required to play.

On first launch choose your supported original ULES00719 ISO. The program
verifies it and remembers its path. Keep that ISO available for future runs.
A missing/moved ISO returns to selection. No game image or extracted game
asset is included in this release.

Settings, saves, cache and logs: %LOCALAPPDATA%\\Asphalt2Recomp
Gameplay: W/Up accelerate; S/Down brake; A/Left and D/Right steer; Space uses NOS.
Menus: arrows or WASD navigate; Space confirms. Enter pauses gameplay.
New Windows profiles: 4x, Device native, 60 FPS, Auto Accelerate Off, FPS Counter Off, Original UI.
Existing saved preferences are retained.
XInput controllers are supported. F11 toggles fullscreen.
Close the window to exit. Use the in-game Recomp Settings for native 30/60
presentation and display/input settings.

If launch fails, check the message and the log under the data folder above.
Install the current graphics driver from your GPU vendor if OpenGL is missing.

FFmpeg 7.1.4 is dynamically linked under LGPL 2.1 or later. You may replace
the three compatible x64 DLLs beside Asphalt2.exe with your rebuilt versions.
No executable signing or integrity check restricts replacing these DLLs.
See ffmpeg-source/REBUILD.txt for the exact source, configuration and recipe.
See licenses/ for all bundled third-party notices.
"""

REBUILD = """FFmpeg shared-library source and rebuild instructions

This release uses FFmpeg 7.1.4 (LGPL version 2.1 or later), without GPL or
nonfree components. The upstream source archive is included here unchanged:
ffmpeg-7.1.4.tar.xz
SHA-256: 71f4aac3573ed9060489cb62526a6c7dda815ae10993789611acd7be9fa9fbf4
Upstream: https://ffmpeg.org/releases/ffmpeg-7.1.4.tar.xz

The exact Python recipe used for the included DLLs is build_ffmpeg_windows.py.
It extracts into a separate output directory, configures the minimal H.264,
ATRAC3/ATRAC3+ and swscale libraries, builds them and records hashes in build.json.
configuration.json records the actual configuration and binary hashes.

Install Python 3.12+ and obtain the official w64devkit x64 2.11.0 toolchain
(GCC 16.2.0). From this folder run, substituting your w64devkit bin directory:

  python build_ffmpeg_windows.py --archive ffmpeg-7.1.4.tar.xz --toolchain "C:\\w64devkit\\bin" --output rebuilt

The compatible replacement DLLs appear under rebuilt/install/bin/. Close the
game and replace avcodec-61.dll, avutil-59.dll and swscale-8.dll beside
Asphalt2.exe. Keep those ABI major versions and build for Windows x64.
The player imports the public FFmpeg API dynamically; no relink of the player
is required. Modifying/replacing FFmpeg for your own use and debugging such
modifications is permitted. No FFmpeg source modifications were made.

Full licenses are in ../licenses and inside the source archive. This package
includes the corresponding FFmpeg source directly, so no later source offer or
network access is needed to obtain it. Compiler binaries are not bundled.
"""


def validate_archive_names(names):
    """A whitelist also rejects unknown files, rather than guessing all secrets."""
    exact = {'Asphalt2.exe', 'README.txt', 'release.json', 'SHA256SUMS.txt',
             'licenses/THIRD_PARTY_LICENSES.txt', 'licenses/psprecomp-MIT.txt',
             'licenses/FFmpeg-LGPLv2.1.txt', 'licenses/FFmpeg-LICENSE.md',
             'licenses/MinGW-w64-runtime.txt', 'ffmpeg-source/REBUILD.txt',
             f'ffmpeg-source/ffmpeg-{VERSION}.tar.xz', 'ffmpeg-source/build_ffmpeg_windows.py',
             'ffmpeg-source/configuration.json'}
    seen = set()
    for name in names:
        if name in seen or str(PurePosixPath(name)) != name or '\\' in name or name.startswith('/'):
            raise ValueError('Unsafe/duplicate archive path: ' + name)
        seen.add(name)
        if name not in exact and not re.fullmatch(r'(?:avcodec-61|avutil-59|swscale-8)\.dll', name):
            raise ValueError('Unapproved Windows release file: ' + name)


def package(output=DEFAULT_OUTPUT):
    report = json.loads((BUILD / 'build.json').read_text(encoding='utf-8'))
    exe = BUILD / 'Asphalt2.exe'
    if sha(exe) != report['sha256']:
        raise ValueError('Executable differs from the verified Windows build')
    if any(not (ROOT / name).is_file() or sha(ROOT / name) != digest
           for name, digest in report['source_sha256'].items()):
        raise ValueError('Sources changed after the Windows build; rebuild before packaging')
    dlls = [BUILD / name for name in report['dll_sha256']]
    if len(dlls) != 3 or any(sha(path) != report['dll_sha256'][path.name] for path in dlls):
        raise ValueError('FFmpeg DLLs differ from the verified build')
    dependencies = audit_dependencies([exe, *dlls])
    if dependencies != report['dependencies'] or dependencies[exe.name]['subsystem'] != 2:
        raise ValueError('Windows dependency/architecture audit differs')
    source = ROOT / f'toolchains/ffmpeg-{VERSION}.tar.xz'
    if sha(source) != ARCHIVE_SHA256:
        raise ValueError('FFmpeg source differs from the pinned archive')
    payload = {exe.name: exe.read_bytes(), **{p.name: p.read_bytes() for p in dlls},
               'README.txt': README.encode(), 'ffmpeg-source/REBUILD.txt': REBUILD.encode(),
               f'ffmpeg-source/ffmpeg-{VERSION}.tar.xz': source.read_bytes(),
               'ffmpeg-source/build_ffmpeg_windows.py': (ROOT / 'tools/build_ffmpeg_windows.py').read_bytes(),
               'licenses/THIRD_PARTY_LICENSES.txt': (ROOT / 'THIRD_PARTY_LICENSES.txt').read_bytes(),
               'licenses/psprecomp-MIT.txt': (ROOT / 'vendor/psprecomp/LICENSE').read_bytes(),
               'licenses/MinGW-w64-runtime.txt': (ROOT / 'toolchains/w64devkit/COPYING.MinGW-w64-runtime.txt').read_bytes()}
    with tarfile.open(source, 'r:xz') as archive:
        for original, target in [('COPYING.LGPLv2.1', 'FFmpeg-LGPLv2.1.txt'), ('LICENSE.md', 'FFmpeg-LICENSE.md')]:
            payload['licenses/' + target] = archive.extractfile(f'ffmpeg-{VERSION}/' + original).read()
    payload['ffmpeg-source/configuration.json'] = json.dumps(report['ffmpeg'], indent=2).encode()
    public_report = {key: report[key] for key in ['version', 'platform', 'sha256', 'bytes', 'generated_units',
                     'compiled_units', 'registration_calls', 'registration_order_preserved', 'compiler',
                     'compiler_sha256', 'dll_sha256', 'dependencies', 'manifest_embedded', 'startup_image_embedded']}
    public_report['ffmpeg_source_sha256'] = ARCHIVE_SHA256
    public_report['game_assets_included'] = False
    public_report['dll_replacement_supported'] = True
    payload['release.json'] = json.dumps(public_report, indent=2).encode()
    payload['SHA256SUMS.txt'] = ''.join(hashlib.sha256(data).hexdigest() + '  ' + name + '\n'
                                        for name, data in sorted(payload.items())).encode()
    validate_archive_names(payload)
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    # Deterministic timestamps/order; rebuilding an unchanged package is stable.
    temporary = output.with_suffix(output.suffix + '.tmp')
    with zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(payload.items()):
            info = zipfile.ZipInfo(name, (2026, 11, 11, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data, compresslevel=9)
    with zipfile.ZipFile(temporary) as archive:
        validate_archive_names(archive.namelist())
        if archive.testzip() is not None or any(archive.read(name) != data for name, data in payload.items()):
            raise ValueError('Archive readback verification failed')
    temporary.replace(output)
    result = {'archive': str(output), 'sha256': sha(output), 'bytes': output.stat().st_size,
              'entries': len(payload), 'executable_sha256': report['sha256'], 'dll_sha256': report['dll_sha256'],
              'game_assets_included': False, 'ffmpeg_source_included': True, 'archive_readback_verified': True}
    (BUILD / 'package.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    print(json.dumps(package(args.output), indent=2))


if __name__ == '__main__':
    main()
