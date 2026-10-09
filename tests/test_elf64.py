"""Regression coverage for native ELF parsing and address-bound constants."""

import contextlib
import io
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import elf64
from check_project import parse_c_constant

START = 0x100000000
END = 0x200000000


def executable():
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    header = elf64.HEADER.pack(ident, 2, 62, 1, START, 64, 0, 0,
                              64, 56, 1, 0, 0, 0)
    program = elf64.PROGRAM.pack(1, 5, 120, START, START, 1, 1, 1)
    return bytearray(header + program + b"\xc3")


class Elf64Tests(unittest.TestCase):
    def test_full_width_executable(self):
        self.assertEqual(elf64.load_end(executable(), START, END), START + 1)

    def test_truncated_files(self):
        for size in [0, 4, 52, 63, 64, 119, 120]:
            with self.subTest(size=size), self.assertRaises(ValueError):
                elf64.load_end(executable()[:size], START, END)

    def test_wrong_architecture_and_header_sizes(self):
        for offset, fmt, value in [(4, "B", 1), (5, "B", 2), (6, "B", 0),
                                   (16, "H", 3), (18, "H", 3), (52, "H", 52),
                                   (54, "H", 32), (56, "H", 0)]:
            data = executable()
            struct.pack_into("<" + fmt, data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                elf64.load_end(data, START, END)

    def test_invalid_segment_ranges_and_entry(self):
        # Offsets here follow ELF64, including the flags before p_offset.
        for offset, value in [(32, elf64.UINT64_MAX), (72, elf64.UINT64_MAX),
                              (80, START - 1), (80, END), (96, 2),
                              (104, elf64.UINT64_MAX), (24, START + 1)]:
            data = executable()
            struct.pack_into("<Q", data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                elf64.load_end(data, START, END)
        data = executable()
        struct.pack_into("<I", data, 68, 4)  # Entry segment must be executable.
        with self.assertRaises(ValueError):
            elf64.load_end(data, START, END)

    def test_section_range_above_four_gib(self):
        data = executable()
        data.extend(bytes(7))  # section table begins at 128
        names = b"\0.text\0.shstrtab\0"
        struct.pack_into("<Q", data, 40, 128)
        struct.pack_into("<HHH", data, 58, 64, 3, 2)
        data += elf64.SECTION.pack(0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
        data += elf64.SECTION.pack(1, 1, 6, START, 120, 1, 0, 0, 1, 0)
        data += elf64.SECTION.pack(7, 3, 0, 0, 320, len(names), 0, 0, 1, 0)
        data += names
        self.assertEqual(elf64.section_range(data, ".text"), (START, START + 1))
        with self.assertRaises(ValueError):
            elf64.section_range(data[:-1], ".text")

    def test_constants_aliases_and_derived_windows(self):
        definitions = """#define BASE UINT64_C(0x100000000)
#define START BASE
#define SIZE 64u
#define END (START + SIZE * 4096)
"""
        self.assertEqual(parse_c_constant(definitions, "END"), START + 64 * 4096)
        for definitions in ["#define A B\n#define B A", "#define A __import__('os')"]:
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                parse_c_constant(definitions, "A")


if __name__ == "__main__":
    unittest.main()
