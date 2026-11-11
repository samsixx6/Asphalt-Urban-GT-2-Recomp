"""Prepare a local fixed-address analysis image from Asphalt's PSP PRX."""
from collections import Counter
import hashlib
import struct


def rebase_prx(blob: bytes, load_base: int) -> tuple[bytes, dict]:
    """Relocate this game's single-segment PRX; never modify the source dump.

    This deliberately supports only the four relocation types found in this
    disc. HI16 uses the next matching LO16's *original*, signed immediate.
    Relocation offsets remain as absolute analysis metadata in the ET_EXEC
    result; an ET_EXEC loader must not apply these relocations a second time.
    """
    def inside(offset, size):
        if offset < 0 or size < 0 or offset + size > len(blob):
            raise ValueError('ELF table or data lies outside the input')

    inside(0, 52)
    if blob[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('expected little-endian ELF32')
    header = struct.unpack_from('<HHIIIIIHHHHHH', blob, 16)
    kind, machine, _, entry, phoff, shoff, _, ehsize, phsize, phnum, shsize, shnum, _ = header
    if kind != 0xFFA0 or machine != 8:
        raise ValueError('expected an unrelocated Allegrex PRX')
    if ehsize != 52 or phsize != 32 or shsize != 40:
        raise ValueError('unexpected ELF table entry size')
    inside(phoff, phsize * phnum)
    inside(shoff, shsize * shnum)
    segments = [struct.unpack_from('<8I', blob, phoff + i * phsize) for i in range(phnum)]
    loads = [(i, seg) for i, seg in enumerate(segments) if seg[0] == 1]
    if len(loads) != 1 or loads[0][0] != 0 or loads[0][1][2] != 0:
        raise ValueError('only one zero-based PT_LOAD segment is supported')
    segment = loads[0][1]
    _, file_offset, _, _, file_size, memory_size, _, _ = segment
    inside(file_offset, file_size)
    if file_size > memory_size or not 0 <= entry < file_size:
        raise ValueError('invalid load segment or entry point')
    if load_base & 0xFFF or not 0x08800000 <= load_base < load_base + memory_size <= 0x0A000000:
        raise ValueError('load segment must fit in aligned PSP user RAM')
    sections = [struct.unpack_from('<10I', blob, shoff + i * shsize) for i in range(shnum)]
    result = bytearray(blob)
    counts = Counter()
    patched = set()
    for section in sections:
        _, section_type, _, _, rel_offset, rel_size, _, _, _, rel_entry_size = section
        if section_type in (9, 0x700000A1) and rel_size:
            raise ValueError('unsupported relocation section encoding')
        if section_type != 0x700000A0:
            continue
        inside(rel_offset, rel_size)
        if rel_size % 8 or rel_entry_size not in (0, 8):
            raise ValueError('invalid PSP relocation entry size')
        records = []
        for pos in range(rel_offset, rel_offset + rel_size, 8):
            offset, info = struct.unpack_from('<II', blob, pos)
            relocation_type = info & 255
            source_segment, target_segment = (info >> 8) & 255, (info >> 16) & 255
            if source_segment != 0 or target_segment != 0 or info >> 24:
                raise ValueError('unsupported relocation segment reference')
            if relocation_type not in (2, 4, 5, 6):
                raise ValueError(f'unsupported relocation type {relocation_type}')
            if offset + 4 > file_size:
                raise ValueError('relocation patch lies outside the file-backed segment')
            if offset in patched:
                raise ValueError('duplicate relocation patch address')
            patched.add(offset)
            word = struct.unpack_from('<I', blob, file_offset + offset)[0]
            records.append((pos, offset, info, relocation_type, word))
        # The PSP table can contain several HI16 entries sharing one LO16.
        next_low = {}
        pairs = {}
        for pos, _, info, relocation_type, word in reversed(records):
            identity = info >> 8
            if relocation_type == 6:
                immediate = word & 0xFFFF
                next_low[identity] = immediate - 0x10000 if immediate & 0x8000 else immediate
            elif relocation_type == 5:
                if identity not in next_low:
                    raise ValueError('unpaired HI16 relocation')
                pairs[pos] = next_low[identity]
        for pos, offset, _, relocation_type, word in records:
            if relocation_type == 2:
                relocated = (word + load_base) & 0xFFFFFFFF
            elif relocation_type == 4:
                relocated = (word & 0xFC000000) | (((word & 0x03FFFFFF) + (load_base >> 2)) & 0x03FFFFFF)
            elif relocation_type == 5:
                address = ((word & 0xFFFF) << 16) + pairs[pos] + load_base
                relocated = (word & 0xFFFF0000) | (((address + 0x8000) >> 16) & 0xFFFF)
            else:
                relocated = (word & 0xFFFF0000) | ((word + load_base) & 0xFFFF)
            struct.pack_into('<I', result, file_offset + offset, relocated)
            struct.pack_into('<I', result, pos, offset + load_base)
            counts[relocation_type] += 1
    if not counts:
        raise ValueError('PRX has no supported relocation records')
    struct.pack_into('<H', result, 16, 2)  # ET_EXEC: already relocated.
    struct.pack_into('<I', result, 24, entry + load_base)
    struct.pack_into('<II', result, phoff + 8, load_base, load_base)
    for index, section in enumerate(sections):
        if section[2] & 2:  # SHF_ALLOC, including zero-filled .bss.
            if section[3] + section[5] > memory_size:
                raise ValueError('allocated section lies outside the load segment')
            struct.pack_into('<I', result, shoff + index * shsize + 12, section[3] + load_base)
    output = bytes(result)
    return output, {
        'load_base': f'0x{load_base:08X}', 'entry': f'0x{entry + load_base:08X}',
        'memory_end': f'0x{load_base + memory_size:08X}',
        'relocations_applied': sum(counts.values()),
        'relocations_by_type': {str(key): value for key, value in sorted(counts.items())},
        'source_sha256': hashlib.sha256(blob).hexdigest(),
        'output_sha256': hashlib.sha256(output).hexdigest(),
        'input_preserved': True, 'output_type': 'ET_EXEC',
    }
