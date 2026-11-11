"""Public-source checks; no game, compiler, downloaded dependency or secret required."""
import hashlib
import importlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile


class ReleaseSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp_root = Path(__file__).resolve().parents[1] / 'build/tests/release-source'
        cls.temp_root.mkdir(parents=True, exist_ok=True)

    def temporary_directory(self):
        return tempfile.TemporaryDirectory(dir=self.temp_root)

    def module(self, name):
        try:
            return importlib.import_module(name)
        except ModuleNotFoundError:
            self.fail(f'{name} must implement the public source workflow')

    def test_prepare_rejects_unsupported_disc_before_output(self):
        prep = self.module('prepare_release_inputs')
        with self.temporary_directory() as tmp:
            root = Path(tmp)
            disc = root / 'unrelated.iso'
            disc.write_bytes(b'not the supported original')
            with self.assertRaisesRegex(ValueError, 'supported ISO'):
                prep.prepare_inputs(root / 'clean', disc, metadata_only=True)
            self.assertFalse((root / 'clean').exists())

    def test_iso_reader_bounds_directory_records(self):
        prep = self.module('prepare_release_inputs')
        with self.temporary_directory() as tmp:
            path = Path(tmp) / 'bad.iso'
            data = bytearray(18 * 2048)
            data[16 * 2048:16 * 2048 + 7] = b'\x01CD001\x01'
            struct.pack_into('<H', data, 16 * 2048 + 128, 2048)
            record = 16 * 2048 + 156
            data[record] = 34
            struct.pack_into('<I', data, record + 2, 17)
            struct.pack_into('<I', data, record + 10, 2048)
            data[17 * 2048] = 12  # too small even for a directory record header
            path.write_bytes(data)
            with path.open('rb') as stream:
                with self.assertRaisesRegex(ValueError, 'directory record'):
                    prep.iso_files(stream)

    def test_registration_split_preserves_calls_and_rejects_code(self):
        prep = self.module('prepare_release_inputs')
        with self.temporary_directory() as tmp:
            root = Path(tmp)
            gen = root / 'work/generated-ctors'
            gen.mkdir(parents=True)
            calls = ['psp_register(0x08804000u, psp_func_08804000);',
                     'psp_register_label(0x08804004u, psp_at_08804004);']
            (gen / 'asphalt_funcs.h').write_text('void psp_func_08804000(void);\nvoid psp_at_08804004(void);\n')
            register = gen / 'asphalt_register.c'
            register.write_text('void psp_recomp_register(void) {\n' + '\n'.join(calls) + '\n}\n')
            report = prep.split_registration(root, chunk_size=1)
            self.assertEqual(report['calls_preserved'], 2)
            self.assertEqual(report['parts'], 2)
            parts = sorted((root / 'build/registration-split').glob('*part*.c'))
            self.assertEqual([line.strip() for path in parts for line in path.read_text().splitlines()
                              if line.strip() in calls], calls)
            register.write_text('void psp_recomp_register(void) { dangerous(); }')
            with self.assertRaisesRegex(ValueError, 'Unexpected registration'):
                prep.split_registration(root)

    def test_export_allowlist_does_not_admit_private_tree_or_artwork(self):
        export = self.module('export_source')
        allowed = ['src/android_game.c', 'src/windows_main.c', 'android/app/src/main/java/com/example/App.java',
                   'vendor/psprecomp/src/hle/ge.c', 'vendor/psprecomp/LICENSE', 'tools/prepare_release_inputs.py']
        denied = ['build/asphalt_import_catalog.h', 'work/generated-ctors/asphalt_0.c', 'work/input/BOOT.BIN',
                  'vendor/psprecomp/.git/config', 'toolchains/w64devkit/bin/gcc.exe', 'README.md',
                  'STATUS.md', 'src/native-game-fixture.json', 'src/test_nos_v23_original_bytes.c',
                  'android/app/src/main/res/drawable-nodpi/app_icon.png', 'windows/Asphalt2.ico',
                  'capture/ram.dump', 'saves/DATA.BIN', 'android/test-signing.keystore', 'unknown.py']
        for name in allowed:
            self.assertTrue(export.is_public_path(name), name)
        for name in denied:
            self.assertFalse(export.is_public_path(name), name)

    def test_export_is_deterministic_and_includes_verified_vendor_sources(self):
        export = self.module('export_source')
        with self.temporary_directory() as tmp:
            root = Path(tmp)
            for name, data in {'src/android_game.c': b'/* shared source */\n',
                               'vendor/psprecomp/src/cpu.c': b'/* donor */\n',
                               'vendor/psprecomp/LICENSE': b'MIT License\n',
                               'tools/build_apk.py': b"APP_ICON_SHA = 'old'\n    signer_sha = '" + b'a' * 64 + b"'\n",
                               'docs/github-README.md': b'# Public source\n',
                               'work/input/BOOT.BIN': b'private game data',
                               'build/test-signing.keystore': b'private key'}.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            first, second = root / 'first.zip', root / 'second.zip'
            export.export_source(root, first)
            export.export_source(root, second)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            with zipfile.ZipFile(first) as archive:
                names = archive.namelist()
                self.assertIn('prototype/README.md', names)
                manifest = json.loads(archive.read('prototype/vendor/psprecomp/SOURCE_MANIFEST.json'))
                self.assertEqual(manifest['revision'], '0ad357fed8fd4a59226034ff6e61aaba2a0d27bb')
                self.assertEqual(manifest['files']['src/cpu.c'], hashlib.sha256(b'/* donor */\n').hexdigest())
                self.assertIn('prototype/android/app/src/main/res/drawable-nodpi/app_icon.png', names)
                self.assertTrue(all('BOOT' not in name and 'keystore' not in name for name in names))

    def test_public_apk_guard_uses_its_own_local_certificate(self):
        export = self.module('export_source')
        original = "APP_ICON_SHA = 'old'\n    signer_sha = '" + 'a' * 64 + "'\n    assert ('Signer #1 certificate SHA-256 digest: ' + signer_sha) in signature\n"
        self.assertTrue(hasattr(export, 'public_apk_script'), 'Public APK needs a local signing identity guard')
        updated = export.public_apk_script(original, 'f' * 64)
        self.assertIn("'-exportcert'", updated)
        self.assertIn('hashlib.sha256(key_certificate).hexdigest()', updated)
        self.assertNotIn('a' * 64, updated)
        self.assertIn("assert ('Signer #1 certificate SHA-256 digest: ' + signer_sha) in signature", updated)
        self.assertEqual(updated, export.public_apk_script(updated, 'f' * 64))

    def test_vendor_manifest_rejects_changed_source(self):
        source = self.module('source_provenance')
        with self.temporary_directory() as tmp:
            vendor = Path(tmp)
            (vendor / 'src').mkdir()
            (vendor / 'src/cpu.c').write_bytes(b'original')
            manifest = {'revision': source.PIN, 'files': {'src/cpu.c': hashlib.sha256(b'original').hexdigest()}}
            (vendor / 'SOURCE_MANIFEST.json').write_text(json.dumps(manifest))
            self.assertEqual(source.verify_exported_vendor(vendor), source.PIN)
            (vendor / 'src/cpu.c').write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'differs'):
                source.verify_exported_vendor(vendor)


if __name__ == '__main__':
    unittest.main()
