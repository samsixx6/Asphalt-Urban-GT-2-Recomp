"""Build the pinned minimal FFmpeg shared libraries locally, without downloads."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]
VERSION = '7.1.4'
ARCHIVE_SHA256 = '71f4aac3573ed9060489cb62526a6c7dda815ae10993789611acd7be9fa9fbf4'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def configure_options(prefix):
    return [f'--prefix={prefix}', '--target-os=mingw32', '--arch=x86_64', '--cc=gcc',
            '--extra-cflags=-mcrtdll=ucrt', '--extra-ldflags=-mcrtdll=ucrt -static-libgcc',
            '--disable-everything', '--disable-autodetect', '--disable-x86asm',
            '--disable-doc', '--disable-programs', '--disable-network',
            '--disable-avdevice', '--disable-avfilter', '--disable-avformat',
            '--disable-postproc', '--disable-swresample', '--disable-debug',
            '--enable-shared', '--disable-static',
            '--enable-decoder=h264,atrac3,atrac3p', '--enable-parser=h264', '--enable-swscale']


def build(archive, toolchain, output, jobs=4):
    archive, toolchain, output = archive.resolve(), toolchain.resolve(), output.resolve()
    if not archive.is_file() or sha(archive) != ARCHIVE_SHA256:
        raise ValueError('FFmpeg source archive is missing or differs from the pinned SHA-256')
    if not (toolchain / 'gcc.exe').is_file():
        raise ValueError('w64devkit bin directory must contain gcc.exe')
    source_parent = output / 'source'
    source = source_parent / f'ffmpeg-{VERSION}'
    install = output / 'install'
    logs = output / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    recipe_hash = sha(Path(__file__))
    report_path = output / 'build.json'
    if report_path.is_file():
        cached = json.loads(report_path.read_text(encoding='utf-8'))
        if (cached.get('recipe_sha256') == recipe_hash and
                cached.get('compiler_sha256') == sha(toolchain / 'gcc.exe') and
                cached.get('libraries') and all((install / name).is_file() and
                    sha(install / name) == digest for name, digest in cached['libraries'].items())):
            return cached
    if not source.is_dir():
        source_parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive, 'r:xz') as stream:
            stream.extractall(source_parent, filter='data')
    env = dict(os.environ)
    env['PATH'] = str(toolchain) + os.pathsep + env.get('PATH', '')
    (source / 'fftmp').mkdir(exist_ok=True)
    env['TMPDIR'] = './fftmp'
    options = configure_options('../../install')
    started = time.monotonic()
    for label, command in [
        ('configure', [str(toolchain / 'sh.exe'), './configure', *options]),
        ('make', [str(toolchain / 'make.exe'), f'-j{jobs}', 'V=1']),
        ('install', [str(toolchain / 'make.exe'), 'install', 'V=1']),
    ]:
        with (logs / (label + '.log')).open('wb') as log:
            result = subprocess.run(command, cwd=source, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=900)
        if result.returncode:
            raise RuntimeError(label + ':\n' + (logs / (label + '.log')).read_text(errors='replace')[-6000:])
        print(f'Windows FFmpeg {label}: passed', flush=True)
    config = (source / 'config.h').read_text(encoding='utf-8')
    if '#define FFMPEG_LICENSE "LGPL version 2.1 or later"' not in config:
        raise ValueError('Unexpected FFmpeg license configuration')
    files = [*sorted((install / 'bin').glob('*.dll')), *sorted((install / 'lib').glob('*.dll.a'))]
    if len([p for p in files if p.suffix == '.dll']) != 3:
        raise ValueError('Expected exactly avcodec, avutil and swscale DLLs')
    report = {'version': VERSION, 'archive_sha256': ARCHIVE_SHA256,
              'source_url': f'https://ffmpeg.org/releases/ffmpeg-{VERSION}.tar.xz',
              'recipe_sha256': recipe_hash, 'compiler_sha256': sha(toolchain / 'gcc.exe'),
              'compiler': subprocess.check_output([str(toolchain / 'gcc.exe'), '--version'], env=env, text=True).splitlines()[0],
              'configuration': options, 'license': 'LGPL version 2.1 or later',
              'libraries': {p.relative_to(install).as_posix(): sha(p) for p in files},
              'seconds': round(time.monotonic() - started, 2)}
    report_path.write_text(json.dumps(report, indent=2), encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, default=ROOT / f'toolchains/ffmpeg-{VERSION}.tar.xz')
    parser.add_argument('--toolchain', type=Path, default=ROOT / 'toolchains/w64devkit/bin')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/windows/ffmpeg')
    parser.add_argument('--jobs', type=int, choices=range(1, 17), default=4)
    args = parser.parse_args()
    print(json.dumps(build(args.archive, args.toolchain, args.output, args.jobs), indent=2))


if __name__ == '__main__':
    main()
