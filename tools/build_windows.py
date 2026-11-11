"""Compile the current shared sources into an independent portable Windows x64 player."""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time

from build_ffmpeg_windows import build as build_ffmpeg, VERSION as FFMPEG_VERSION
from display_sources import DISPLAY_SOURCES
from patch_display_hooks import prepare

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build/windows'
VENDOR = ROOT / 'vendor/psprecomp'
GEN = ROOT / 'work/generated-ctors'
BIN = ROOT / 'toolchains/w64devkit/bin'
BOOT_SHA256 = 'd2a2e78ae0a19b4d7ac2eeb6386b8ae16cd7c35c624bdd59d0c6b4d241b31f9c'
IMAGE_SHA256 = '6e52686f5f0b9e476a2481661b91ae241bd7b429e060f197d9728565a6317c3b'
RUNTIME = ['cpu', 'mem', 'dispatch', 'ctors', 'render', 'render_gl', 'vfpu']
HLE = ['hle', 'sysmem', 'threadman', 'power', 'intrman', 'display', 'ge', 'sascore', 'iofilemgr', 'misc']
HOST = ['windows_main', 'windows_bootstrap', 'windows_game', 'startup_services', 'savedata',
        'host_interrupts', 'desktop_host', 'audio_mixer', 'host_input', 'host_perf', 'desktop_gl',
        'asphalt_mpeg', 'asphalt_atrac', *DISPLAY_SOURCES]
NATIVE_GAME_FLAGS = ['-O1', '-foptimize-sibling-calls', '-ffp-contract=off']
SYSTEM_DLLS = {'advapi32.dll', 'bcrypt.dll', 'comdlg32.dll', 'crypt32.dll', 'gdi32.dll',
               'kernel32.dll', 'msvcrt.dll', 'ntdll.dll', 'ole32.dll', 'oleaut32.dll',
               'opengl32.dll', 'psapi.dll', 'rpcrt4.dll', 'secur32.dll', 'setupapi.dll',
               'shell32.dll', 'shlwapi.dll', 'ucrtbase.dll', 'user32.dll', 'userenv.dll',
               'version.dll', 'winmm.dll', 'ws2_32.dll', 'xinput1_4.dll'}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def split_registration(source, header, output, chunk_size=2048):
    signature = 'void psp_recomp_register(void) {'
    if source.count(signature) != 1:
        raise ValueError('Unexpected registration entry point')
    body = source.split(signature, 1)[1].rsplit('}', 1)[0]
    calls = [line.strip() for line in body.splitlines() if line.strip()]
    pattern = r'psp_register(?:_label)?\(0x[0-9A-Fa-f]+u, psp_(?:func|at|import)_[0-9A-Fa-f]+\);'
    if not calls or not all(re.fullmatch(pattern, line) for line in calls):
        raise ValueError('Unexpected registration code; refusing an unsafe split')
    declarations = set(re.findall(r'void (psp_(?:func|at|import)_[0-9A-Fa-f]+)\(void\);', header))
    required = {re.search(r', (psp_\w+)\);', line).group(1) for line in calls}
    if not required <= declarations:
        raise ValueError('Generated header is missing registration declarations')
    output.mkdir(parents=True, exist_ok=True)
    paths, names = [], []
    for index, start in enumerate(range(0, len(calls), chunk_size)):
        name = f'asphalt_register_part_{index:03d}'
        path = output / (name + '.c')
        path.write_text('#include "asphalt_funcs.h"\nvoid ' + name + '(void) {\n' +
                        '\n'.join('    ' + call for call in calls[start:start + chunk_size]) + '\n}\n', encoding='utf-8')
        paths.append(path)
        names.append(name)
    main = output / 'asphalt_register.c'
    main.write_text(''.join(f'void {name}(void);\n' for name in names) +
                    'void psp_recomp_register(void) {\n' + ''.join(f'    {name}();\n' for name in names) + '}\n', encoding='utf-8')
    if [call for path in paths for call in re.findall(pattern, path.read_text())] != calls:
        raise ValueError('Registration split changed the original order')
    return [*paths, main], calls


def pe_details(data):
    def number(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data):
            raise ValueError('Truncated PE image')
        return struct.unpack_from(fmt, data, offset)[0]
    if data[:2] != b'MZ':
        raise ValueError('Not a PE image')
    pe = number('<I', 0x3c)
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('Invalid PE signature')
    machine = number('<H', pe + 4)
    count = number('<H', pe + 6)
    optional_size = number('<H', pe + 20)
    optional = pe + 24
    if number('<H', optional) != 0x20b or optional_size < 128:
        raise ValueError('Expected PE32+ image')
    sections = []
    for index in range(count):
        section = optional + optional_size + index * 40
        sections.append((number('<I', section + 12), number('<I', section + 16), number('<I', section + 20)))
    def rva(value, size=1):
        for virtual, raw_size, raw in sections:
            offset = value - virtual
            if 0 <= offset and offset + size <= raw_size and raw + offset + size <= len(data):
                return raw + offset
        raise ValueError('Unmapped PE RVA')
    imports = []
    import_rva, import_size = struct.unpack_from('<II', data, optional + 120)
    if import_rva:
        if import_size < 20 or import_size > len(data):
            raise ValueError('Invalid PE import directory')
        for index in range(import_size // 20):
            descriptor = rva(import_rva + index * 20, 20)
            fields = struct.unpack_from('<IIIII', data, descriptor)
            if not any(fields):
                break
            start = rva(fields[3])
            end = data.find(b'\0', start, min(start + 512, len(data)))
            if end < 0:
                raise ValueError('Unterminated PE import name')
            try:
                name = data[start:end].decode('ascii').lower()
            except UnicodeDecodeError as exc:
                raise ValueError('Invalid PE import name') from exc
            if not re.fullmatch(r'[a-z0-9_.-]+\.dll', name):
                raise ValueError('Invalid PE import name')
            imports.append(name)
        else:
            raise ValueError('Unterminated PE import directory')
    return {'machine': 'x86_64' if machine == 0x8664 else hex(machine),
            'subsystem': number('<H', optional + 68), 'imports': sorted(set(imports))}


def audit_dependencies(paths):
    bundled = {path.name.lower() for path in paths}
    report = {}
    for path in paths:
        details = pe_details(path.read_bytes())
        if details['machine'] != 'x86_64':
            raise ValueError(path.name + ' must be Windows x64')
        missing = [name for name in details['imports'] if name not in bundled and name not in SYSTEM_DLLS
                   and not name.startswith(('api-ms-win-', 'ext-ms-win-'))]
        if missing:
            raise ValueError(path.name + ' has unbundled dependencies: ' + ', '.join(missing))
        report[path.name] = details
    return report


def compile_resources(compiler_bin, resource_dir, output, env):
    # windres builds a shell command for its default absolute preprocessor;
    # using gcc from the explicitly prepared PATH avoids breaking on spaces.
    return subprocess.run([str(compiler_bin / 'windres.exe'), '--preprocessor=gcc',
                           '--preprocessor-arg=-E', '--preprocessor-arg=-xc', '--preprocessor-arg=-DRC_INVOKED',
                           '-I', '.', '-i', 'asphalt.rc', '-o', str(output.resolve())],
                          cwd=resource_dir, env=env, capture_output=True, timeout=60)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, choices=range(1, 17), default=4)
    args = parser.parse_args()
    started = time.monotonic()
    if not (BIN / 'gcc.exe').is_file():
        raise SystemExit('Run tools/setup_windows.py first: portable compiler is missing')
    for relative, expected in [('work/input/BOOT.BIN', BOOT_SHA256), ('work/input/ASPHALT_REBASED.ELF', IMAGE_SHA256)]:
        if not (ROOT / relative).is_file() or sha(ROOT / relative) != expected:
            raise SystemExit('Missing/unsupported input: ' + relative + '; run prepare_release_inputs.py --iso <supported ISO>')
    relocation = json.loads((ROOT / 'work/relocation-results.json').read_text())
    if relocation['source_sha256'] != BOOT_SHA256 or relocation['output_sha256'] != IMAGE_SHA256:
        raise SystemExit('Relocation metadata differs from supported input')
    for relative in ['build/asphalt_import_catalog.h', 'build/asphalt_missing_guards.h',
                     'windows/asphalt.rc', 'windows/app.manifest', 'windows/Asphalt2.ico']:
        if not (ROOT / relative).is_file():
            raise SystemExit('Missing required build input: ' + relative)
    objects = BUILD / 'objects'
    logs = BUILD / 'logs'
    objects.mkdir(parents=True, exist_ok=True)
    logs.mkdir(parents=True, exist_ok=True)
    ffmpeg = BUILD / 'ffmpeg'
    ffmpeg_report = build_ffmpeg(ROOT / f'toolchains/ffmpeg-{FFMPEG_VERSION}.tar.xz', BIN, ffmpeg, args.jobs)
    install = ffmpeg / 'install'
    split, calls = split_registration((GEN / 'asphalt_register.c').read_text(encoding='utf-8'),
                                     (GEN / 'asphalt_funcs.h').read_text(encoding='utf-8'), BUILD / 'registration-split')
    overrides = prepare()
    patched = BUILD / 'display-generated'
    patched.mkdir(exist_ok=True)
    for name, path in overrides.items():
        shutil.copyfile(path, patched / name)
    game_sources = [(patched / path.name if path.name in overrides else path, 'game')
                    for path in sorted(GEN.glob('*.c')) if path.name != 'asphalt_register.c']
    if not game_sources:
        raise SystemExit('Generated game sources are missing; prepare the supported ISO first')
    sources = [*game_sources, *((p, 'register') for p in split),
               *((VENDOR / f'src/{name}.c', 'core') for name in RUNTIME),
               *((VENDOR / f'src/hle/{name}.c', 'hle') for name in HLE),
               *((ROOT / f'src/{name}.c', 'host') for name in HOST)]
    flags = ['-mcrtdll=ucrt', '-std=c11', *NATIVE_GAME_FLAGS, '-DPSPRECOMP_REAL_THREADS=1',
             '-DPSPRECOMP_TRACE=1', '-DASPHALT_WINDOWS_RELEASE=1',
             '-I', str(VENDOR / 'include'), '-I', str(GEN), '-I', str(ROOT / 'build'), '-I', str(ROOT / 'src')]
    headers = sorted(set([*VENDOR.rglob('*.h'), *sorted((ROOT / 'src').glob('*.h')),
                         *sorted((ROOT / 'build').glob('*.h')), GEN / 'asphalt_funcs.h']))
    tracked = [*headers, *(path for path, _ in sources), ROOT / 'src/display-hooks.json',
               GEN / 'asphalt_register.c', ROOT / 'windows/asphalt.rc', ROOT / 'windows/app.manifest', ROOT / 'windows/Asphalt2.ico',
               Path(__file__), ROOT / 'tools/build_ffmpeg_windows.py', ROOT / 'tools/patch_display_hooks.py', ROOT / 'tools/display_sources.py']
    source_hashes = {p.relative_to(ROOT).as_posix(): sha(p) for p in tracked}
    common = hashlib.sha256(json.dumps(flags).encode() + bytes.fromhex(sha(BIN / 'gcc.exe')) +
                            b''.join(bytes.fromhex(sha(path)) for path in headers)).digest()
    env = dict(os.environ)
    env['PATH'] = str(BIN) + os.pathsep + env.get('PATH', '')
    def compile_one(item):
        path, kind = item
        label = kind + '_' + path.stem
        obj = objects / (label + '.o')
        stamp = obj.with_suffix('.sha256')
        extra = ['-DASPHALT_FFMPEG=1', '-I', str(install / 'include')] if path.stem in ('asphalt_mpeg', 'asphalt_atrac') else []
        digest = hashlib.sha256(common + path.read_bytes() + json.dumps(extra).encode() +
                                (json.dumps(ffmpeg_report['libraries'], sort_keys=True).encode() if extra else b'')).hexdigest()
        if obj.is_file() and stamp.is_file() and stamp.read_text() == digest:
            return obj
        result = subprocess.run([str(BIN / 'gcc.exe'), *flags, *extra, '-c', str(path), '-o', str(obj)], env=env, capture_output=True, timeout=600)
        (logs / (label + '.log')).write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(path.name + ':\n' + result.stderr.decode(errors='replace')[-7000:])
        stamp.write_text(digest)
        return obj
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(compile_one, item) for item in sources]
        ready = []
        for future in concurrent.futures.as_completed(futures):
            ready.append(future.result())
            if len(ready) % 50 == 0 or len(ready) == len(sources):
                print(f'Windows current-source units: {len(ready)}/{len(sources)}', flush=True)
    resource = objects / 'windows_resource.o'
    result = compile_resources(BIN, ROOT / 'windows', resource, env)
    (logs / 'resources.log').write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors='replace'))
    exe = BUILD / 'Asphalt2.exe'
    import_libraries = [install / f'lib/lib{name}.dll.a' for name in ('swscale', 'avcodec', 'avutil')]
    link = ['-mcrtdll=ucrt', '-municode', '-mwindows', '-static-libgcc',
            *map(str, sorted(ready)), str(resource), *map(str, import_libraries),
            '-lwinmm', '-luser32', '-lgdi32', '-lopengl32', '-lbcrypt', '-lcomdlg32', '-lshell32', '-lole32', '-lm', '-o', str(exe)]
    response = BUILD / 'link.rsp'
    response.write_text('\n'.join('"' + arg.replace('\\', '/') + '"' for arg in link), encoding='utf-8')
    result = subprocess.run([str(BIN / 'gcc.exe'), '@' + str(response)], env=env, capture_output=True, timeout=300)
    (logs / 'link.log').write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors='replace')[-10000:])
    dlls = []
    for path in sorted((install / 'bin').glob('*.dll')):
        destination = BUILD / path.name
        shutil.copyfile(path, destination)
        dlls.append(destination)
    dependency_report = audit_dependencies([exe, *dlls])
    if dependency_report[exe.name]['subsystem'] != 2:
        raise ValueError('Player must use the Windows GUI subsystem')
    if (ROOT / 'windows/app.manifest').read_bytes() not in exe.read_bytes():
        raise ValueError('Windows application manifest was not embedded')
    # A changing source tree cannot silently produce a mixed-version release.
    if any(sha(ROOT / name) != digest for name, digest in source_hashes.items()):
        raise ValueError('Source changed during compilation; rerun the builder')
    report = {'version': '1.0', 'platform': 'Windows x64', 'executable': 'build/windows/Asphalt2.exe',
              'sha256': sha(exe), 'bytes': exe.stat().st_size, 'generated_units': len(game_sources),
              'compiled_units': len(sources), 'registration_calls': len(calls),
              'registration_order_preserved': True, 'flags': flags,
              'compiler': subprocess.check_output([str(BIN / 'gcc.exe'), '--version'], env=env, text=True).splitlines()[0],
              'compiler_sha256': sha(BIN / 'gcc.exe'), 'source_sha256': source_hashes,
              'dll_sha256': {p.name: sha(p) for p in dlls}, 'dependencies': dependency_report,
              'ffmpeg': ffmpeg_report, 'manifest_embedded': True, 'startup_image_embedded': False,
              'seconds': round(time.monotonic() - started, 2)}
    (BUILD / 'build.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({k: v for k, v in report.items() if k not in ('source_sha256', 'flags', 'ffmpeg')}, indent=2), flush=True)


if __name__ == '__main__':
    main()
