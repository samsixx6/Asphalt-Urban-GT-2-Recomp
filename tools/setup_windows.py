"""Download exact local Windows build dependencies and verify before extraction."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def provision(root=ROOT, offline=False):
    root = Path(root).resolve()
    packages = json.loads((root / 'metadata/dependencies.json').read_text(encoding='utf-8'))['windows']
    tools = root / 'toolchains'
    tools.mkdir(parents=True, exist_ok=True)
    reports = []
    for package in packages:
        archive = tools / package['archive']
        if not archive.exists():
            if offline:
                raise ValueError('Missing verified offline archive: ' + str(archive))
            partial = archive.with_suffix(archive.suffix + '.download')
            with urllib.request.urlopen(package['url'], timeout=120) as response, partial.open('wb') as output:
                while block := response.read(1024 * 1024):
                    output.write(block)
            if hashlib.sha256(partial.read_bytes()).hexdigest() != package['sha256']:
                raise ValueError('Downloaded archive checksum differs: ' + package['archive'])
            partial.replace(archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != package['sha256']:
            raise ValueError('Archive checksum differs: ' + package['archive'])
        if package['name'] == 'w64devkit':
            compiler = tools / 'w64devkit/bin/gcc.exe'
            if not compiler.is_file():
                subprocess.run([str(archive), '-y', '-o' + str(tools)], check=True, timeout=180)
            if not compiler.is_file():
                raise ValueError('Compiler not found after w64devkit extraction')
            version = subprocess.check_output([str(compiler), '--version'], text=True).splitlines()[0]
            if '16.2.0' not in version:
                raise ValueError('Unexpected w64devkit compiler: ' + version)
        else:
            source = tools / 'ffmpeg-7.1.4'
            if not source.exists():
                with tarfile.open(archive, 'r:xz') as tar:
                    tar.extractall(tools, filter='data')
        reports.append({key: package[key] for key in ('name', 'version', 'url', 'sha256')})
        print(package['name'] + ': verified', flush=True)
    (root / 'build').mkdir(exist_ok=True)
    (root / 'build/windows-toolchain.json').write_text(json.dumps(reports, indent=2), encoding='utf-8')
    return reports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--offline', action='store_true', help='Use only checksum-matching archives already in toolchains/')
    options = parser.parse_args()
    try:
        provision(offline=options.offline)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))


if __name__ == '__main__':
    main()
