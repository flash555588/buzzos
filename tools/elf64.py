"""Read little-endian x86_64 executables without running their contents."""

import struct

HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
PROGRAM = struct.Struct("<IIQQQQQQ")
SECTION = struct.Struct("<IIQQQQIIQQ")
UINT64_MAX = (1 << 64) - 1


def file_range(data, offset, size):
    """Reject truncated ranges before unpacking or slicing."""
    if offset > len(data) or size > len(data) - offset:
        raise ValueError("range outside file")


def header(data):
    """Validate and return an ELF64 executable header."""
    file_range(data, 0, HEADER.size)
    fields = HEADER.unpack_from(data)
    ident, kind, machine, version = fields[:4]
    if (ident[:4] != b"\x7fELF" or ident[4:7] != bytes((2, 1, 1))
            or kind != 2 or machine != 62 or version != 1):
        raise ValueError("expected a little-endian x86_64 ELF64 executable")
    if fields[8] != HEADER.size or fields[9] != PROGRAM.size or not fields[10]:
        raise ValueError("invalid ELF/program-header size or count")
    file_range(data, fields[5], fields[9] * fields[10])
    return fields


def load_end(data, start=0, end=UINT64_MAX):
    """Validate all load segments and the executable entry; return image end."""
    fields = header(data)
    entry, phoff, count = fields[4], fields[5], fields[10]
    highest = 0
    entry_ok = False
    saw_load = False
    for index in range(count):
        kind, flags, offset, address, _, filesz, memsz, _ = PROGRAM.unpack_from(
            data, phoff + index * PROGRAM.size)
        if kind != 1:
            continue
        saw_load = True
        if filesz > memsz:
            raise ValueError("PT_LOAD filesz exceeds memsz")
        file_range(data, offset, filesz)
        if memsz > UINT64_MAX - address:
            raise ValueError("PT_LOAD address overflows uint64")
        segment_end = address + memsz
        if address < start or segment_end > end:
            raise ValueError("PT_LOAD outside user load window")
        highest = max(highest, segment_end)
        if flags & 1 and address <= entry < segment_end:
            entry_ok = True
    if not saw_load or not entry_ok:
        raise ValueError("missing executable loadable entry segment")
    return highest


def section_range(data, wanted):
    """Return the virtual range of a named kernel section."""
    fields = header(data)
    offset, stride, count, strings = fields[6], fields[11], fields[12], fields[13]
    if stride != SECTION.size or strings >= count:
        raise ValueError("invalid section headers")
    file_range(data, offset, stride * count)
    names_section = SECTION.unpack_from(data, offset + strings * stride)
    names_offset, names_size = names_section[4:6]
    file_range(data, names_offset, names_size)
    names = data[names_offset:names_offset + names_size]
    for index in range(count):
        section = SECTION.unpack_from(data, offset + index * stride)
        name_offset, address, size = section[0], section[3], section[5]
        if name_offset >= len(names):
            raise ValueError("section name outside string table")
        name_end = names.find(b"\0", name_offset)
        if name_end < 0:
            raise ValueError("unterminated section name")
        if size > UINT64_MAX - address:
            raise ValueError("section address overflows uint64")
        if names[name_offset:name_end].decode("ascii") == wanted:
            return address, address + size
    raise ValueError(f"missing section {wanted}")
