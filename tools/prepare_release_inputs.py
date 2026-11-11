"""Regenerate private game build inputs from the exact supported, user-owned ISO.

No parent research folder or captured runtime state is required. All generated
game material stays in ignored work/ and build/ directories.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

from prx_image import rebase_prx

ROOT = Path(__file__).resolve().parents[1]
ISO_SHA256 = '64ab1ba788baade948b9b89d47145a653c063e85afed403b89d917d8a8703ec4'
ISO_BYTES = 884146176
BOOT_SHA256 = 'd2a2e78ae0a19b4d7ac2eeb6386b8ae16cd7c35c624bdd59d0c6b4d241b31f9c'
IMAGE_SHA256 = '6e52686f5f0b9e476a2481661b91ae241bd7b429e060f197d9728565a6317c3b'
LOAD_BASE = 0x08804000
REGISTRATION = r'psp_register(?:_label)?\(0x[0-9A-Fa-f]+u, psp_(?:func|at|import)_[0-9A-Fa-f]+\);'


def file_hash(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def iso_files(stream):
    """Read bounded ISO9660 directory metadata; never extract an arbitrary path."""
    stream.seek(0, 2)
    total = stream.tell()

    def read(offset, size):
        if offset < 0 or size < 0 or size > total or offset > total - size:
            raise ValueError('ISO extent lies outside input')
        stream.seek(offset)
        data = stream.read(size)
        if len(data) != size:
            raise ValueError('Short ISO read')
        return data

    pvd = read(16 * 2048, 2048)
    if pvd[:7] != b'\x01CD001\x01' or struct.unpack_from('<H', pvd, 128)[0] != 2048:
        raise ValueError('Expected ISO9660 with 2048-byte sectors')
    if pvd[156] < 34:
        raise ValueError('Invalid ISO root directory record')
    pending = [('', struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0])]
    seen, result = set(), []
    while pending:
        parent, lba, length = pending.pop()
        if (lba, length) in seen:
            continue
        seen.add((lba, length))
        if len(seen) > 4096 or length > 16 * 1024 * 1024:
            raise ValueError('ISO directory limit exceeded')
        data = read(lba * 2048, length)
        pos = 0
        while pos < len(data):
            size = data[pos]
            if not size:
                pos = (pos // 2048 + 1) * 2048
                continue
            if size < 34 or pos + size > len(data) or pos % 2048 + size > 2048:
                raise ValueError('Malformed ISO directory record')
            record = data[pos:pos + size]
            pos += size
            if 33 + record[32] > size:
                raise ValueError('Malformed ISO directory record name')
            name = record[33:33 + record[32]]
            if name in (b'\x00', b'\x01'):
                continue
            name = name.decode('ascii').split(';', 1)[0]
            if not name or any(char in name for char in '/\\') or name in ('.', '..'):
                raise ValueError('Unsafe ISO directory name')
            path = (parent + '/' + name).lstrip('/')
            extent, count = struct.unpack_from('<I', record, 2)[0], struct.unpack_from('<I', record, 10)[0]
            if extent * 2048 > total - count:
                raise ValueError('ISO file extent lies outside input')
            if record[25] & 2:
                pending.append((path, extent, count))
            else:
                result.append({'path': path, 'lba': extent, 'size': count})
    return sorted(result, key=lambda item: item['path'])


def parse_imports(boot, names):
    """Import locations and NIDs come from BOOT; names are PSP ABI metadata."""
    shoff = struct.unpack_from('<I', boot, 32)[0]
    shsize, shnum, shstr = struct.unpack_from('<HHH', boot, 46)
    sections = [struct.unpack_from('<10I', boot, shoff + i * shsize) for i in range(shnum)]
    strings = sections[shstr]
    strings = boot[strings[4]:strings[4] + strings[5]]

    def offset(address, count=1):
        for section in sections:
            if section[2] & 2 and section[1] != 8 and section[3] <= address <= section[3] + section[5] - count:
                return section[4] + address - section[3]
        raise ValueError('Import address outside file-backed ELF sections')

    def cstring(address):
        start = offset(address)
        end = boot.find(b'\x00', start, min(len(boot), start + 256))
        if end < start:
            raise ValueError('Unterminated import library name')
        return boot[start:end].decode('ascii')

    module = next(section for section in sections
                  if strings[section[0]:strings.find(b'\x00', section[0])] == b'.rodata.sceModuleInfo')
    stub_top, stub_end = struct.unpack_from('<II', boot, module[4] + 44)
    imports = []
    while stub_top < stub_end:
        name_ptr, _, _, words, variables, count, nids, stubs = struct.unpack_from('<IHHBBHII', boot, offset(stub_top, 20))
        if words < 5 or stub_top + words * 4 > stub_end or variables:
            raise ValueError('Unsupported import table record')
        library = cstring(name_ptr)
        entries = []
        for index in range(count):
            nid = struct.unpack_from('<I', boot, offset(nids + index * 4, 4))[0]
            key = f'0x{nid:08X}'
            entries.append({'nid': key, 'name': names.get(key, key), 'stub_address': hex(stubs + index * 8)})
        imports.append({'library': library, 'entries': entries})
        stub_top += words * 4
    return imports


def write_import_headers(root, imports):
    entries = [entry | {'library': library['library']} for library in imports for entry in library['entries']]
    catalog = ('/* Local metadata generated from the inspected import table. */\n'
               'typedef struct { unsigned nid; const char *library, *name; } asphalt_import;\n'
               'static const asphalt_import asphalt_imports[] = {\n'
               + ''.join(f'    {{{entry["nid"]}u, {json.dumps(entry["library"])}, {json.dumps(entry["name"])}}},\n' for entry in entries)
               + '};\n#define ASPHALT_IMPORT_COUNT (sizeof(asphalt_imports)/sizeof(asphalt_imports[0]))\n')
    guards = ''.join(f'static void missing_{i}(void) {{ missing_import({i}); }}\n' for i in range(len(entries)))
    guards += 'static void install_missing_guards(void) {\n'
    guards += ''.join(f'    if (!psp_hle_name(asphalt_imports[{i}].nid)) '
                      f'psp_hle_register(asphalt_imports[{i}].nid, asphalt_imports[{i}].library, '
                      f'asphalt_imports[{i}].name, missing_{i});\n' for i in range(len(entries)))
    guards += '}\n'
    build = root / 'build'
    build.mkdir(parents=True, exist_ok=True)
    (build / 'asphalt_import_catalog.h').write_text(catalog, encoding='utf-8')
    (build / 'asphalt_missing_guards.h').write_text(guards, encoding='utf-8')
    return len(entries)


def split_registration(root, chunk_size=2048):
    root = Path(root)
    generated = root / 'work/generated-ctors'
    text = (generated / 'asphalt_register.c').read_text(encoding='utf-8')
    body = text.split('void psp_recomp_register(void) {', 1)[1].rsplit('}', 1)[0]
    calls = [line.strip() for line in body.splitlines() if line.strip()]
    if not calls or not all(re.fullmatch(REGISTRATION, line) for line in calls):
        raise ValueError('Unexpected registration code; refusing an unsafe split')
    declarations = set(re.findall(r'void (psp_(?:func|at|import)_[0-9A-Fa-f]+)\(void\);',
                                  (generated / 'asphalt_funcs.h').read_text(encoding='utf-8')))
    required = {re.search(r', (psp_\w+)\);', line).group(1) for line in calls}
    if not required <= declarations:
        raise ValueError('Generated header is missing registration declarations')
    target = root / 'build/registration-split'
    target.mkdir(parents=True, exist_ok=True)
    names = []
    for index, start in enumerate(range(0, len(calls), chunk_size)):
        name = f'asphalt_register_part_{index:03d}'
        names.append(name)
        (target / (name + '.c')).write_text('#include "asphalt_funcs.h"\nvoid ' + name + '(void) {\n'
            + '\n'.join('    ' + call for call in calls[start:start + chunk_size]) + '\n}\n', encoding='utf-8')
    for obsolete in target.glob('asphalt_register_part_*.c'):
        if obsolete.stem not in names:
            obsolete.unlink()
    (target / 'asphalt_register.c').write_text(''.join(f'void {name}(void);\n' for name in names)
        + 'void psp_recomp_register(void) {\n' + ''.join(f'    {name}();\n' for name in names) + '}\n', encoding='utf-8')
    reproduced = [line.strip() for name in names for line in (target / (name + '.c')).read_text().splitlines()
                  if re.fullmatch(REGISTRATION, line.strip())]
    if reproduced != calls:
        raise ValueError('Registration order changed during splitting')
    report = {'verified': True, 'calls_preserved': len(calls), 'parts': len(names),
              'original_order_preserved': True, 'game_instructions_changed': False}
    (root / 'build/registration-split-verification.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    return report


def prepare_inputs(root, iso, metadata_only=False):
    root, iso = Path(root).resolve(), Path(iso).resolve()
    if iso.stat().st_size != ISO_BYTES or file_hash(iso) != ISO_SHA256:
        raise ValueError('Expected the supported ISO (ULES00719 Europe, SHA-256 ' + ISO_SHA256 + ')')
    with iso.open('rb') as stream:
        files = iso_files(stream)
        entry = next(item for item in files if item['path'].upper() == 'PSP_GAME/SYSDIR/BOOT.BIN')
        stream.seek(entry['lba'] * 2048)
        boot = stream.read(entry['size'])
    if hashlib.sha256(boot).hexdigest() != BOOT_SHA256:
        raise ValueError('Original BOOT hash differs')
    image, relocation = rebase_prx(boot, LOAD_BASE)
    if hashlib.sha256(image).hexdigest() != IMAGE_SHA256:
        raise ValueError('Rebased image hash differs')
    constructor_entries = struct.unpack_from('<511I', image, 0x08B36030 - LOAD_BASE + 96)
    if len(set(constructor_entries)) != 511 or any(addr & 3 or not 0x08AFD444 <= addr < 0x08B36030 for addr in constructor_entries):
        raise ValueError('Constructor table differs from the supported layout')
    names = json.loads((root / 'metadata/psp-import-names.json').read_text(encoding='utf-8'))
    imports = parse_imports(boot, names)
    work = root / 'work'
    inputs = work / 'input'
    inputs.mkdir(parents=True, exist_ok=True)
    for name, data in [('BOOT.BIN', boot), ('ASPHALT_REBASED.ELF', image)]:
        path = inputs / name
        if path.exists() and path.read_bytes() != data:
            raise ValueError('Existing local input differs; preserving ' + name)
        path.write_bytes(data)
    seeds = inputs / 'constructor-seeds.txt'
    seeds.write_text(''.join(f'0x{address:08X}\n' for address in constructor_entries), encoding='ascii')
    (work / 'relocation-results.json').write_text(json.dumps(relocation, indent=2), encoding='utf-8')
    count = write_import_headers(root, imports)
    report = {'iso_path': str(iso), 'iso_sha256': ISO_SHA256, 'boot_sha256': BOOT_SHA256,
              'image_sha256': IMAGE_SHA256, 'imports': count, 'constructors': 511,
              'game_executed': False, 'generated': False}
    if not metadata_only:
        tool = root / 'build/allegrexrecomp.exe'
        if not tool.is_file():
            subprocess.run([sys.executable, str(root / 'tools/build_tool.py')], cwd=root, check=True)
        logs = work / 'logs'
        logs.mkdir(exist_ok=True)
        # Stage the generated files so a failed emit cannot leave mixed output.
        with tempfile.TemporaryDirectory(prefix='release-emit-', dir=work) as temp:
            command = [str(tool), 'emit', str(inputs / 'ASPHALT_REBASED.ELF'), temp, 'asphalt',
                       '--strict-exec', '--exec-range', '0x08AFD444:0x08B36030', '--seeds', str(seeds)]
            with (logs / 'release-emit.log').open('wb') as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600)
            stage = Path(temp)
            profile = json.loads((root / 'src/display-hooks.json').read_text(encoding='utf-8'))
            for name, spec in profile['units'].items():
                if hashlib.sha256((stage / name).read_text(encoding='utf-8').encode()).hexdigest() != spec['sha256']:
                    raise ValueError('Fresh generated hook source differs: ' + name)
            generated = work / 'generated-ctors'
            generated.mkdir(exist_ok=True)
            staged_names = {path.name for path in stage.iterdir() if path.is_file()}
            for path in generated.iterdir():
                if path.is_file() and path.suffix in ('.c', '.h') and path.name not in staged_names:
                    path.unlink()
            for path in stage.iterdir():
                if path.is_file():
                    (generated / path.name).write_bytes(path.read_bytes())
        report['registration'] = split_registration(root)
        report['generated'] = True
    (root / 'build/release-inputs.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, required=True, help='Path to the supported original Europe ULES00719 ISO')
    parser.add_argument('--metadata-only', action='store_true', help='Verify ISO/rebase/imports; omit generated C and registration')
    options = parser.parse_args()
    try:
        report = prepare_inputs(ROOT, options.iso, options.metadata_only)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
