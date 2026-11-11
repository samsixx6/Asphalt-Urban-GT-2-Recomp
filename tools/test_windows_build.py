"""Windows release builder/package invariants; no game or compiler is needed."""
import importlib
import os
from pathlib import Path
import struct
import tempfile
import unittest


def pe_image(machine=0x8664, subsystem=2, imports=()):
    """A tiny PE32+ fixture with a real import directory and mapped section."""
    data = bytearray(0x800)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', data, 0x84, machine, 1, 0, 0, 0, 240, 0x22)
    struct.pack_into('<H', data, 0x98, 0x20b)
    struct.pack_into('<H', data, 0x98 + 68, subsystem)
    struct.pack_into('<I', data, 0x98 + 108, 16)
    struct.pack_into('<II', data, 0x98 + 120, 0x1000, 20 * (len(imports) + 1))
    section = 0x98 + 240
    data[section:section + 8] = b'.rdata\0\0'
    struct.pack_into('<IIII', data, section + 8, 0x600, 0x1000, 0x600, 0x200)
    for i, name in enumerate(imports):
        location = 0x400 + i * 0x80
        struct.pack_into('<IIIII', data, 0x200 + 20 * i, 0, 0, 0, 0x1000 + location - 0x200, 0)
        encoded = name.encode('ascii') + b'\0'
        data[location:location + len(encoded)] = encoded
    return bytes(data)


class WindowsReleaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.base = Path(__file__).resolve().parent
        cls.temp_root = cls.base.parent / 'build/tests/windows-build'
        cls.temp_root.mkdir(parents=True, exist_ok=True)

    def temporary_directory(self):
        return tempfile.TemporaryDirectory(dir=self.temp_root)

    def load(self, name):
        self.assertTrue((self.base / (name + '.py')).is_file(), name + ' is not implemented')
        return importlib.import_module(name)

    def test_registration_split_preserves_every_call_and_order(self):
        builder = self.load('build_windows')
        header = 'void psp_func_08800000(void);\nvoid psp_at_08800004(void);\n'
        source = 'void psp_recomp_register(void) {\npsp_register(0x08800000u, psp_func_08800000);\npsp_register_label(0x08800004u, psp_at_08800004);\n}\n'
        with self.temporary_directory() as temp:
            paths, calls = builder.split_registration(source, header, Path(temp), chunk_size=1)
            self.assertEqual(calls, ['psp_register(0x08800000u, psp_func_08800000);', 'psp_register_label(0x08800004u, psp_at_08800004);'])
            parts = [p for p in paths if '_part_' in p.name]
            self.assertEqual(len(parts), 2)
            self.assertIn(calls[0], parts[0].read_text())
            self.assertIn(calls[1], parts[1].read_text())

    def test_registration_refuses_unrecognized_code(self):
        builder = self.load('build_windows')
        with self.temporary_directory() as temp, self.assertRaisesRegex(ValueError, 'registration'):
            builder.split_registration('void psp_recomp_register(void) { dangerous(); }', '', Path(temp))

    def test_registration_refuses_missing_declaration(self):
        builder = self.load('build_windows')
        with self.temporary_directory() as temp, self.assertRaisesRegex(ValueError, 'declaration'):
            builder.split_registration('void psp_recomp_register(void) {\npsp_register(0x08800000u, psp_func_08800000);\n}', '', Path(temp))

    def test_pe_reader_reports_architecture_subsystem_and_imports(self):
        builder = self.load('build_windows')
        details = builder.pe_details(pe_image(imports=['KERNEL32.dll', 'avcodec-61.dll']))
        self.assertEqual(details['machine'], 'x86_64')
        self.assertEqual(details['subsystem'], 2)
        self.assertEqual(details['imports'], ['avcodec-61.dll', 'kernel32.dll'])

    def test_pe_reader_rejects_malformed_rva(self):
        builder = self.load('build_windows')
        data = bytearray(pe_image(imports=['KERNEL32.dll']))
        struct.pack_into('<I', data, 0x200 + 12, 0xffffffff)
        with self.assertRaisesRegex(ValueError, 'RVA'):
            builder.pe_details(bytes(data))

    def test_dependency_audit_checks_transitive_dlls(self):
        builder = self.load('build_windows')
        with self.temporary_directory() as temp:
            root = Path(temp)
            exe = root / 'Asphalt2.exe'
            dll = root / 'avcodec-61.dll'
            exe.write_bytes(pe_image(imports=['avcodec-61.dll', 'ucrtbase.dll']))
            dll.write_bytes(pe_image(imports=['missing-codec.dll']))
            with self.assertRaisesRegex(ValueError, 'missing-codec.dll'):
                builder.audit_dependencies([exe, dll])

    def test_dependency_audit_refuses_non_x64_dll(self):
        builder = self.load('build_windows')
        with self.temporary_directory() as temp:
            path = Path(temp) / 'wrong.dll'
            path.write_bytes(pe_image(machine=0x14c))
            with self.assertRaisesRegex(ValueError, 'x64'):
                builder.audit_dependencies([path])

    def test_archive_guard_refuses_game_and_private_files(self):
        package = self.load('package_windows')
        for name in ['BOOT.BIN', 'data/image.elf', 'game.iso', 'generated/asphalt_000.c', 'save.ini', '../bad.txt', 'objects/game.o', 'key.keystore']:
            with self.subTest(name=name), self.assertRaises(ValueError):
                package.validate_archive_names([name])
        package.validate_archive_names(['Asphalt2.exe', 'avcodec-61.dll', 'README.txt', 'licenses/FFmpeg-LGPLv2.1.txt', 'ffmpeg-source/ffmpeg-7.1.4.tar.xz', 'ffmpeg-source/build_ffmpeg_windows.py'])

    def test_ffmpeg_recipe_preserves_lgpl_shared_build(self):
        recipe = self.load('build_ffmpeg_windows')
        options = recipe.configure_options('../install')
        self.assertIn('--enable-shared', options)
        self.assertIn('--disable-static', options)
        self.assertIn('--disable-autodetect', options)
        self.assertNotIn('--enable-gpl', options)
        self.assertNotIn('--enable-nonfree', options)
        self.assertEqual(recipe.ARCHIVE_SHA256, '71f4aac3573ed9060489cb62526a6c7dda815ae10993789611acd7be9fa9fbf4')

    def test_resource_compile_handles_paths_with_spaces(self):
        builder = self.load('build_windows')
        compiler_bin = self.base.parent / 'toolchains/w64devkit/bin'
        if not (compiler_bin / 'windres.exe').is_file():
            self.skipTest('Resource integration test needs the optional portable toolchain')
        self.assertTrue(hasattr(builder, 'compile_resources'), 'Space-safe resource compiler is not implemented')
        env = dict(os.environ)
        env['PATH'] = str(compiler_bin) + os.pathsep + env.get('PATH', '')
        with tempfile.TemporaryDirectory(prefix='resource path ', dir=self.temp_root) as temp:
            obj = Path(temp) / 'windows resource.o'
            result = builder.compile_resources(compiler_bin, self.base.parent / 'windows', obj, env)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
            self.assertGreater(obj.stat().st_size, 1000)


if __name__ == '__main__':
    unittest.main()
