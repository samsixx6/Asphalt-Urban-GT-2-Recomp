"""Verify the pinned, locally patched donor in a source-only exported tree."""
import hashlib
import json
from pathlib import Path
import subprocess

PIN = '0ad357fed8fd4a59226034ff6e61aaba2a0d27bb'


def verify_exported_vendor(vendor):
    vendor = Path(vendor).resolve()
    manifest = json.loads((vendor / 'SOURCE_MANIFEST.json').read_text(encoding='utf-8'))
    if manifest.get('revision') != PIN or not manifest.get('files'):
        raise ValueError('Unexpected donor source manifest revision or empty file list')
    for relative, expected in manifest['files'].items():
        path = (vendor / relative).resolve()
        if not path.is_relative_to(vendor) or not path.is_file():
            raise ValueError('Missing or unsafe donor source: ' + relative)
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError('Exported donor source differs: ' + relative)
    actual = {path.relative_to(vendor).as_posix() for path in vendor.rglob('*')
              if path.is_file() and path.suffix in ('.c', '.h')}
    if not actual <= set(manifest['files']):
        raise ValueError('Unrecorded donor C source/header in exported tree')
    return PIN


def verify_vendor(vendor):
    vendor = Path(vendor)
    if (vendor / '.git').exists():
        revision = subprocess.check_output(['git', '-C', str(vendor), 'rev-parse', 'HEAD'], text=True).strip()
        if revision != PIN:
            raise ValueError('Unexpected recompiler revision: ' + revision)
        return revision
    return verify_exported_vendor(vendor)
