"""Build the pinned donor's recompiler for this throwaway feasibility probe."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
from source_provenance import verify_vendor

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / 'vendor' / 'psprecomp'
BIN = ROOT / 'toolchains' / 'w64devkit' / 'bin'
BUILD = ROOT / 'build'
PIN = '0ad357fed8fd4a59226034ff6e61aaba2a0d27bb'
RUNTIME = ['cpu', 'mem', 'dispatch', 'ctors', 'render', 'render_gl', 'vfpu', 'traps']
HLE = ['hle', 'sysmem', 'threadman', 'power', 'intrman', 'display', 'ge', 'sascore', 'iofilemgr', 'misc']
TOOL = ['decode', 'container', 'analyze', 'emit', 'keys', 'crypto/aes', 'crypto/cmac', 'crypto/sha1', 'crypto/kirk']
SOURCES = ([f'src/{name}.c' for name in RUNTIME]
           + [f'src/hle/{name}.c' for name in HLE]
           + [f'tools/allegrexrecomp/{name}.c' for name in TOOL])


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    (BUILD / 'objects').mkdir(exist_ok=True)
    (BUILD / 'logs').mkdir(exist_ok=True)
    env = dict(os.environ)
    env['PATH'] = str(BIN) + os.pathsep + env.get('PATH', '')
    gcc = BIN / 'gcc.exe'
    if not gcc.is_file():
        raise SystemExit('The portable compiler has not been extracted')
    revision = verify_vendor(VENDOR)
    if revision != PIN:
        raise SystemExit(f'Unexpected recompiler revision: {revision}')
    # The donor uses C11 timespec_get; w64devkit defaults to the older MSVCRT.
    # Select UCRT for both compilation and linking, as verified by clock-probe.json.
    flags = ['-mcrtdll=ucrt', '-std=c11', '-O1', '-g', '-DPSPRECOMP_REAL_THREADS=1', '-DPSPRECOMP_TRACE=1',
             '-I', str(VENDOR / 'include'), '-I', str(VENDOR / 'tools' / 'allegrexrecomp')]
    started = time.monotonic()

    def compile_one(relative):
        label = relative.replace('/', '_').replace('.c', '')
        obj = BUILD / 'objects' / (label + '.o')
        command = [str(gcc), *flags, '-c', str(VENDOR / relative), '-o', str(obj)]
        result = subprocess.run(command, env=env, capture_output=True, timeout=180)
        (BUILD / 'logs' / (label + '.log')).write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f'Compilation failed: {relative}\n{result.stderr.decode("utf-8", errors="replace")[-5000:]}')
        return obj

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        objects = list(pool.map(compile_one, SOURCES))
    main_obj = compile_one('tools/allegrexrecomp/main.c')

    def link(output, obj):
        result = subprocess.run([str(gcc), '-mcrtdll=ucrt', *map(str, obj), '-lwinmm', '-lm', '-o', str(output)],
                                env=env, capture_output=True, timeout=120)
        (BUILD / 'logs' / (output.stem + '-link.log')).write_bytes(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(result.stderr.decode('utf-8', errors='replace')[-5000:])

    tool = BUILD / 'allegrexrecomp.exe'
    link(tool, [*objects, main_obj])
    tests = []
    for name in ['decode', 'runtime', 'container', 'crypto', 'kirk', 'emit', 'mem_fault', 'exec_ranges', 'host_tls', 'trace_progress', 'scheduler', 'thread_lifecycle', 'hle', 'vfpu', 'ctors', 'raster']:
        obj = compile_one(f'tests/test_{name}.c')
        exe = BUILD / f'test_{name}.exe'
        link(exe, [*objects, obj])
        test_dir = BUILD / 'tests' / name
        test_dir.mkdir(parents=True, exist_ok=True)
        cases = ['delete', 'terminate_exited', 'pinned', 'pending', 'checkpoint0', 'checkpoint1', 'checkpoint2', 'checkpoint3', 'mask', 'reset', 'regs'] if name == 'thread_lifecycle' else [None]
        for case in cases:
            label = name + ('-' + case if case else '')
            result = subprocess.run([str(exe), *([case] if case else [])], env=env, cwd=test_dir, capture_output=True, timeout=60)
            (BUILD / 'logs' / f'test_{label}-run.log').write_bytes(result.stdout + result.stderr)
            tests.append({'name': label, 'exit_code': result.returncode,
                          'output_tail': (result.stdout + result.stderr).decode('utf-8', errors='replace')[-900:]})
            if result.returncode:
                raise RuntimeError(f'Upstream {label} test failed; inspect build/logs')
    patch = subprocess.check_output(['git', '-C', str(VENDOR), 'diff', '--', 'tools', 'src', 'include', 'tests']) if (VENDOR / '.git').exists() else b''
    # git diff omits a new, untracked regression test. Include it so the
    # recorded patch is sufficient to reproduce all local vendor changes.
    new_tests = subprocess.check_output(['git', '-C', str(VENDOR), 'ls-files', '--others', '--exclude-standard', '--', 'tests','include','src'], text=True).splitlines() if (VENDOR / '.git').exists() else []
    for relative in new_tests:
        if not relative.endswith(('.c','.h')):
            continue
        lines = (VENDOR / relative).read_text(encoding='utf-8').splitlines()
        patch += (f'diff --git a/{relative} b/{relative}\nnew file mode 100644\n'
                  f'--- /dev/null\n+++ b/{relative}\n@@ -0,0 +1,{len(lines)} @@\n'
                  + ''.join('+' + line + '\n' for line in lines)).encode('utf-8')
    patch_dir = ROOT / 'patches'
    patch_dir.mkdir(exist_ok=True)
    if (VENDOR / '.git').exists():
        (patch_dir / 'prototype-runtime.patch').write_bytes(patch)
    elif (patch_dir / 'prototype-runtime.patch').exists():
        patch = (patch_dir / 'prototype-runtime.patch').read_bytes()
    report = {'prototype_only': True, 'revision': revision,
              'local_patch_sha256': hashlib.sha256(patch).hexdigest(),
              'compiler': subprocess.check_output([str(gcc), '--version'], env=env, text=True).splitlines()[0],
              'tool': str(tool), 'tool_sha256': hashlib.sha256(tool.read_bytes()).hexdigest(),
              'seconds': round(time.monotonic() - started, 2), 'tests': tests,
              'scope': 'toolkit build and synthetic tests; game execution is recorded separately',
              'android_built': False, 'game_executed': False}
    (BUILD / 'build-manifest.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({**report, 'tests': [{'name': item['name'], 'exit_code': item['exit_code']} for item in tests]}, indent=2), flush=True)


if __name__ == '__main__':
    main()
