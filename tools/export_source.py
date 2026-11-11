"""Create a deterministic, curated public source ZIP for both native targets."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
import zipfile
import zlib

from source_provenance import PIN

ROOT = Path(__file__).resolve().parents[1]
TOOLS = {
    'prepare_release_inputs.py', 'export_source.py', 'source_provenance.py', 'setup_windows.py',
    'build_tool.py', 'prx_image.py', 'build_windows.py', 'package_windows.py', 'build_ffmpeg_windows.py',
    'build_android_native.py', 'build_apk.py', 'setup_android.py', 'build_ffmpeg.py', 'build_ffmpeg_android.py',
    'display_sources.py', 'patch_display_hooks.py', 'test_release_source.py', 'test_prx_image.py',
    'test_windows_bootstrap.py', 'test_windows_launcher.py', 'test_windows_preferences.py', 'test_windows_build.py', 'check_public_source.py',
}
DOCS = {'building.md', 'windows.md', 'github-README.md', 'source-license.md'}
SCREENSHOTS = {
    'Screenshot_20261110_013528.jpg', 'Screenshot_20261110_012830.jpg',
    'Screenshot_20261110_162103.jpg', 'Screenshot_20261110_012944.jpg',
}


def is_public_path(relative):
    path = PurePosixPath(relative.replace('\\', '/'))
    if path.is_absolute() or '..' in path.parts or any(part.startswith('.') for part in path.parts):
        return relative == '.github/workflows/source-checks.yml'
    parts = path.parts
    if not parts:
        return False
    if parts[0] == 'src':
        return len(parts) == 2 and ((path.suffix in ('.c', '.h', '.inc') and
            (not path.name.startswith('test_') or path.name in ('test_windows_bootstrap.c', 'test_windows_launcher.c', 'test_windows_preferences.c'))) or path.name == 'display-hooks.json')
    if parts[0] == 'android':
        prefix = 'android/app/src/main/'
        return relative.startswith(prefix) and (path.suffix in ('.java', '.xml') or
            (str(path.parent) == prefix + 'assets/controls' and path.name in ('NOS.png', 'Brake.png', 'Pause.png', 'Camera.png')))
    if parts[0] == 'tools':
        return len(parts) == 2 and path.name in TOOLS
    if parts[0] == 'docs':
        return len(parts) == 2 and path.name in DOCS
    if parts[0] == 'screenshots':
        return len(parts) == 2 and path.name in SCREENSHOTS
    if parts[:2] == ('vendor', 'psprecomp'):
        rest = parts[2:]
        return bool(rest) and (rest == ('LICENSE',) or (rest[0] in ('include', 'src', 'tests', 'tools') and
            (path.suffix in ('.c', '.h') or path.name == 'CMakeLists.txt')) or rest == ('CMakeLists.txt',))
    if parts[0] == 'windows':
        return path.suffix in ('.rc', '.h', '.manifest')
    if parts[0] == 'metadata':
        return len(parts) == 2 and path.name in ('dependencies.json', 'psp-import-names.json')
    if parts[0] == 'patches':
        return len(parts) == 2 and path.name in ('prototype-runtime.patch', 'strict-executable-analysis.patch')
    return len(parts) == 1 and path.name in ('THIRD_PARTY_LICENSES.txt', 'LICENSE')


def neutral_icon():
    """Original geometric build placeholder. No game or supplied artwork bytes."""
    size = 256
    rows = bytearray()
    for y in range(size):
        rows.append(0)
        for x in range(size):
            color = (19, 29, 47, 255)
            if 42 <= x < 214 and 47 <= y < 171:
                color = (44, 184, 196, 255) if x < 53 or x >= 203 or y < 58 or y >= 160 else (28, 49, 71, 255)
            if (115 <= x < 141 and 171 <= y < 195) or (86 <= x < 170 and 195 <= y < 205):
                color = (44, 184, 196, 255)
            rows.extend(color)

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>2I5B', size, size, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(rows), 9)) + chunk(b'IEND', b'')
    ico = struct.pack('<HHH', 0, 1, 1) + struct.pack('<4BHHII', 0, 0, 0, 0, 1, 32, len(png), 22) + png
    return png, ico


def public_apk_script(text, icon_sha256):
    text, count = re.subn(r"APP_ICON_SHA = '[^']*'", "APP_ICON_SHA = '" + icon_sha256 + "'", text)
    if count != 1:
        raise ValueError('Expected exactly one APK app icon hash guard')
    # Validate against this source user's local key. The private working copy's
    # existing update certificate pin is deliberately untouched.
    replacement = ("    key_certificate = subprocess.check_output([str(JDK / 'keytool.exe'), '-exportcert',\n"
                   "        '-keystore', str(key), '-storepass', 'android', '-alias', 'androiddebugkey'])\n"
                   "    signer_sha = hashlib.sha256(key_certificate).hexdigest()")
    text, count = re.subn(r"    signer_sha = '[0-9a-f]{64}'", lambda _: replacement, text)
    if count != 1 and not (count == 0 and text.count(replacement) == 1):
        raise ValueError('Expected exactly one APK signing identity guard')
    return text


def public_files(root):
    root = Path(root).resolve()
    result = {}
    # Never traverse private work/build/toolchains trees just to filter them later.
    for parent in ('src', 'android', 'tools', 'docs', 'screenshots', 'vendor/psprecomp', 'windows', 'metadata', 'patches', '.github'):
        base = root / parent
        if base.exists():
            for path in base.rglob('*'):
                if path.is_symlink():
                    continue
                relative = path.relative_to(root).as_posix()
                if path.is_file() and is_public_path(relative):
                    result[relative] = path.read_bytes()
    for name in ('THIRD_PARTY_LICENSES.txt', 'LICENSE'):
        if (root / name).is_file():
            result[name] = (root / name).read_bytes()
    readme = root / 'docs/github-README.md'
    if not readme.is_file():
        raise ValueError('Missing curated docs/github-README.md')
    result['README.md'] = readme.read_bytes()
    png, ico = neutral_icon()
    result['android/app/src/main/res/drawable-nodpi/app_icon.png'] = png
    result['windows/Asphalt2.ico'] = ico
    if 'tools/build_apk.py' in result:
        text = result['tools/build_apk.py'].decode('utf-8')
        result['tools/build_apk.py'] = public_apk_script(text, hashlib.sha256(png).hexdigest()).encode('utf-8')
    result['.gitignore'] = (b'/toolchains/\n/work/\n/build/\n*.iso\n*.cso\n*.chd\n*.elf\n*.bin\n'
                            b'*.keystore\n*.jks\n*.apk\n*.exe\n*.dll\n*.so\n*.o\n*.a\n*.zip\n'
                            b'/saves/\n/logs/\n/cache/\n/data/\n__pycache__/\n*.pyc\n')
    result['.gitattributes'] = (b'# Preserve exported bytes for source-manifest checks on every platform.\n'
                               b'* -text\n')
    donor_files = {name.removeprefix('vendor/psprecomp/'): hashlib.sha256(data).hexdigest()
                   for name, data in result.items() if name.startswith('vendor/psprecomp/')}
    result['vendor/psprecomp/SOURCE_MANIFEST.json'] = (json.dumps({
        'upstream': 'https://github.com/sp00nznet/psprecomp', 'revision': PIN,
        'state': 'pinned upstream plus included local runtime/discovery changes',
        'license': 'MIT; see LICENSE', 'files': dict(sorted(donor_files.items()))}, indent=2) + '\n').encode()
    result['SOURCE_MANIFEST.json'] = (json.dumps({'format': 1, 'version': '1.0',
        'content': 'Handwritten shared/Windows/Android source and patched MIT donor. No game data.',
        'artwork': 'Original neutral app/Windows icon; user-supplied touch controls and gameplay screenshots.',
        'files': {name: hashlib.sha256(data).hexdigest() for name, data in sorted(result.items())}}, indent=2) + '\n').encode()
    return result


def export_source(root, output):
    files = public_files(root)
    output = Path(output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for relative, data in sorted(files.items()):
            entry = zipfile.ZipInfo('prototype/' + relative, (2026, 11, 11, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.create_system = 3
            entry.external_attr = 0o100644 << 16
            archive.writestr(entry, data, compresslevel=9)
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None:
            raise ValueError('Source ZIP integrity check failed')
        if len(archive.namelist()) != len(files):
            raise ValueError('Source ZIP file count differs')
    return {'archive': str(output), 'sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
            'files': len(files), 'bytes': output.stat().st_size}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/releases/Asphalt-Urban-GT-2-v1.0-source.zip')
    options = parser.parse_args()
    print(json.dumps(export_source(ROOT, options.output), indent=2))


if __name__ == '__main__':
    main()
