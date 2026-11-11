"""Check exported source manifests and exclusions without requiring the game."""
import hashlib
import json
from pathlib import Path

from source_provenance import verify_exported_vendor

ROOT = Path(__file__).resolve().parents[1]


def check(root=ROOT):
    root = Path(root).resolve()
    manifest_path = root / 'SOURCE_MANIFEST.json'
    if not manifest_path.is_file():
        # The private working tree is allowed to contain ignored game inputs.
        # Check the public projection rather than treating private files as public.
        from export_source import public_files
        files = public_files(root)
        manifest = json.loads(files['SOURCE_MANIFEST.json'])
        for name, expected in manifest['files'].items():
            if hashlib.sha256(files[name]).hexdigest() != expected:
                raise ValueError('Projected source hash differs: ' + name)
    else:
        manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
        for name, expected in manifest['files'].items():
            path = (root / name).resolve()
            if not path.is_relative_to(root) or not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
                raise ValueError('Public source file missing or changed: ' + name)
        verify_exported_vendor(root / 'vendor/psprecomp')
    for name in manifest['files']:
        path = Path(name)
        if path.suffix.lower() in ('.iso', '.cso', '.chd', '.elf', '.bin', '.exe', '.dll', '.so', '.a', '.o', '.keystore', '.jks', '.dump'):
            raise ValueError('Forbidden public source artifact: ' + name)
        if path.parts[0] in ('work', 'build', 'toolchains', 'research', 'reference', 'saves', 'captures'):
            raise ValueError('Forbidden public source tree: ' + name)
    print(f'Public source verified: {len(manifest["files"])} hashed files; no game input required')
    return len(manifest['files'])


if __name__ == '__main__':
    check()
