"""Synthetic fixtures for the relocation cases present in Asphalt's PRX."""
import struct
import unittest

from prx_image import rebase_prx


def fixture(relocations=None):
    data = bytearray(0x500)
    data[:16] = b'\x7fELF\x01\x01\x01' + bytes(9)
    struct.pack_into('<HHIIIIIHHHHHH', data, 16,
                     0xFFA0, 8, 1, 0x10, 52, 0x300, 0, 52, 32, 1, 40, 5, 4)
    struct.pack_into('<8I', data, 52, 1, 0x100, 0, 0, 0xC0, 0x100, 7, 16)
    struct.pack_into('<III', data, 0x100, 0x3C080001, 0x25088004, 0x0C00000C)
    struct.pack_into('<II', data, 0x180, 0x30, 0)
    names = b'\x00.text\x00.data\x00.rel.text\x00.shstrtab\x00'
    data[0x200:0x200 + len(names)] = names
    rels = relocations if relocations is not None else [(0, 5), (4, 6), (8, 4), (0x80, 2)]
    for index, relocation in enumerate(rels):
        struct.pack_into('<II', data, 0x240 + 8 * index, *relocation)
    sections = [(1, 1, 6, 0, 0x100, 0x80),
                (7, 1, 3, 0x80, 0x180, 0x40),
                (13, 0x700000A0, 0, 0, 0x240, len(rels) * 8),
                (23, 3, 0, 0, 0x200, len(names))]
    for index, section in enumerate(sections, 1):
        struct.pack_into('<10I', data, 0x300 + 40 * index, *section, 0, 0, 4, 0)
    return bytes(data)


class RelocationTests(unittest.TestCase):
    def test_relocates_instructions_pointers_and_metadata_without_touching_null(self):
        original = fixture()
        result, report = rebase_prx(original, 0x08804000)
        self.assertEqual(struct.unpack_from('<III', result, 0x100),
                         (0x3C080881, 0x2508C004, 0x0E20100C))
        self.assertEqual(struct.unpack_from('<II', result, 0x180), (0x08804030, 0))
        self.assertEqual(struct.unpack_from('<H', result, 16)[0], 2)
        self.assertEqual(struct.unpack_from('<I', result, 24)[0], 0x08804010)
        self.assertEqual(struct.unpack_from('<I', result, 60)[0], 0x08804000)
        self.assertEqual(struct.unpack_from('<I', result, 0x240)[0], 0x08804000)
        self.assertEqual(struct.unpack_from('<I', result, 0x300 + 40 + 12)[0], 0x08804000)
        self.assertEqual(report['relocations_applied'], 4)
        self.assertEqual(original, fixture())

    def test_multiple_high_halves_share_the_next_signed_low_half(self):
        original = bytearray(fixture([(0, 5), (4, 5), (8, 6)]))
        struct.pack_into('<III', original, 0x100, 0x3C080001, 0x3C090001, 0x25088004)
        result, _ = rebase_prx(bytes(original), 0x08804000)
        self.assertEqual(struct.unpack_from('<III', result, 0x100),
                         (0x3C080881, 0x3C090881, 0x2508C004))

    def test_unsupported_relocation_fails_instead_of_being_skipped(self):
        with self.assertRaisesRegex(ValueError, 'unsupported relocation'):
            rebase_prx(fixture([(0, 7)]), 0x08804000)

    def test_unpaired_high_half_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'unpaired HI16'):
            rebase_prx(fixture([(0, 5)]), 0x08804000)

    def test_bad_segment_reference_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'segment'):
            rebase_prx(fixture([(0, 0x102)]), 0x08804000)

    def test_patch_cannot_run_past_the_segment(self):
        with self.assertRaisesRegex(ValueError, 'outside'):
            rebase_prx(fixture([(0xBF, 2)]), 0x08804000)

    def test_rebased_image_cannot_be_relocated_twice(self):
        result, _ = rebase_prx(fixture(), 0x08804000)
        with self.assertRaisesRegex(ValueError, 'PRX'):
            rebase_prx(result, 0x08804000)


if __name__ == '__main__':
    unittest.main()
