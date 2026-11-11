"""Build only the local media decoders needed by the native feasibility probe."""
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

def main():
    archive = ROOT / 'toolchains' / f'ffmpeg-{VERSION}.tar.xz'
    if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_SHA256:
        raise SystemExit('Official source download differs from the recorded archive')
    source = archive.parent / f'ffmpeg-{VERSION}'
    if not source.exists():
        with tarfile.open(archive, 'r:xz') as tar:
            tar.extractall(archive.parent, filter='data')
    env = dict(os.environ)
    bin_dir = ROOT / 'toolchains/w64devkit/bin'
    env['PATH'] = str(bin_dir) + os.pathsep + env.get('PATH', '')
    # Native Windows TEMP contains backslashes that configure's shell expands.
    # A project-local relative path also avoids spaces in temporary filenames.
    (source / 'fftmp').mkdir(exist_ok=True)
    env['TMPDIR'] = './fftmp'
    logs = ROOT / 'build/logs/ffmpeg'
    logs.mkdir(parents=True, exist_ok=True)
    options = ['--prefix=../ffmpeg-install', '--target-os=mingw32', '--arch=x86_64',
               '--cc=gcc', '--extra-cflags=-mcrtdll=ucrt', '--extra-ldflags=-mcrtdll=ucrt',
               '--disable-everything', '--disable-autodetect', '--disable-x86asm',
               '--disable-doc', '--disable-programs', '--disable-network',
               '--disable-avdevice', '--disable-avfilter', '--disable-avformat',
               '--disable-postproc', '--disable-debug', '--disable-shared', '--enable-static',
               '--enable-decoder=h264,atrac3,atrac3p', '--enable-parser=h264', '--enable-swscale']
    started = time.monotonic()
    commands = [('configure', [str(bin_dir / 'sh.exe'), './configure', *options]),
                ('make', [str(bin_dir / 'make.exe'), '-j4', 'V=1']),
                ('install', [str(bin_dir / 'make.exe'), 'install', 'V=1'])]
    for name, command in commands:
        with (logs / (name + '.log')).open('wb') as log:
            result = subprocess.run(command, cwd=source, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=600)
        if result.returncode:
            print((logs / (name + '.log')).read_text(errors='replace')[-4000:])
            raise SystemExit(result.returncode)
        print(f'FFmpeg {name}: passed', flush=True)
    libs = ROOT / 'toolchains/ffmpeg-install/lib'
    report = {'version': VERSION, 'source_url': f'https://ffmpeg.org/releases/ffmpeg-{VERSION}.tar.xz',
              'archive_sha256': ARCHIVE_SHA256, 'options': options,
              'libraries': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in libs.glob('*.a')},
              'seconds': round(time.monotonic() - started, 2)}
    (ROOT / 'build/ffmpeg-build.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2), flush=True)

if __name__ == '__main__':
    main()
