#!/usr/bin/env python3
import argparse
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SECTOR_SIZE = 512
MINIFS_MAGIC = 0x5346424D
MINIFS_INODES = 2048
MINIFS_DIRECT = 8
MINIFS_NAME_LEN = 24
MINIFS_DIR = 1
MINIFS_FILE = 2
MINIFS_ROOT_INO = 1
MINIFS_DIRENT_SIZE = 32


class FsError(Exception):
    pass


def parse_make_int(name, default):
    makefile = ROOT / "Makefile"
    if not makefile.exists():
        return default
    text = makefile.read_text(encoding="utf-8")
    m = re.search(rf"^{re.escape(name)}\s*:?=\s*(\d+)\s*$", text, re.M)
    return int(m.group(1)) if m else default


def fail(message):
    raise FsError(message)


def ok(message):
    print(f"[ok] {message}")


def warn(message):
    print(f"[warn] {message}")


def read_sector(image, lba):
    off = lba * SECTOR_SIZE
    if off < 0 or off + SECTOR_SIZE > len(image):
        fail(f"LBA {lba} is outside image")
    return image[off:off + SECTOR_SIZE]


def c_name(raw):
    if 0 in raw:
        raw = raw[:raw.index(0)]
    try:
        return raw.decode("ascii")
    except UnicodeDecodeError:
        fail("directory entry name is not ASCII")


class MiniFsImage:
    def __init__(self, image, fs_start, fs_sectors):
        self.image = image
        self.fs_start = fs_start
        self.fs_sectors = fs_sectors
        meta_free = fs_sectors - 1 - MINIFS_INODES
        self.expected_bitmap_sectors = (meta_free + SECTOR_SIZE) // (SECTOR_SIZE + 1)
        self.expected_blocks = meta_free - self.expected_bitmap_sectors
        self.expected_data_lba = (
            fs_start + 1 + MINIFS_INODES + self.expected_bitmap_sectors
        )
        self.super = {}
        self.inodes = []
        self.bitmap = []
        self.block_refs = {}
        self.dir_edges = {}
        self.reachable = set()
        self.paths = {MINIFS_ROOT_INO: "/"}
        self.repairs = []

    def is_empty_region(self):
        off = self.fs_start * SECTOR_SIZE
        size = self.fs_sectors * SECTOR_SIZE
        if off + size > len(self.image):
            fail(f"image is too small for /fs region: need {off + size} bytes")
        return all(b == 0 for b in self.image[off:off + size])

    def inode_offset(self, ino):
        return (self.fs_start + 1 + ino) * SECTOR_SIZE

    def bitmap_offset(self):
        return (self.fs_start + 1 + MINIFS_INODES) * SECTOR_SIZE

    def require_mutable(self):
        if not isinstance(self.image, bytearray):
            fail("repair requires a mutable bytearray image")

    def zero_inode_sector(self, ino, reason):
        self.require_mutable()
        off = self.inode_offset(ino)
        self.image[off:off + SECTOR_SIZE] = bytes(SECTOR_SIZE)
        self.inodes[ino] = {
            "ino": ino,
            "used": 0,
            "type": 0,
            "parent": 0,
            "size": 0,
            "blocks": [0] * MINIFS_DIRECT,
            "indirect": 0,
            "double_indirect": 0,
        }
        self.repairs.append(reason)

    def write_bitmap(self, reason):
        self.require_mutable()
        off = self.bitmap_offset()
        sector = bytearray(self.expected_bitmap_sectors * SECTOR_SIZE)
        for i, value in enumerate(self.bitmap):
            sector[i] = 1 if value else 0
        self.image[off:off + len(sector)] = sector
        self.repairs.append(reason)

    def reset_walk_state(self):
        self.block_refs = {}
        self.dir_edges = {}
        self.reachable = set()
        self.paths = {MINIFS_ROOT_INO: "/"}

    def load(self, allow_invalid_bitmap=False):
        if len(self.image) < (self.fs_start + self.fs_sectors) * SECTOR_SIZE:
            fail("image is smaller than configured filesystem region")
        raw = read_sector(self.image, self.fs_start)
        magic, inode_count, block_count, data_lba = struct.unpack_from("<IIII", raw, 0)
        self.super = {
            "magic": magic,
            "inode_count": inode_count,
            "block_count": block_count,
            "data_lba": data_lba,
        }
        if magic != MINIFS_MAGIC:
            fail(f"bad minifs magic 0x{magic:08X}")
        if inode_count != MINIFS_INODES:
            fail(f"inode_count is {inode_count}, expected {MINIFS_INODES}")
        if block_count != self.expected_blocks:
            fail(f"block_count is {block_count}, expected {self.expected_blocks}")
        if data_lba != self.expected_data_lba:
            fail(f"data_lba is {data_lba}, expected {self.expected_data_lba}")

        self.inodes = []
        for ino in range(MINIFS_INODES):
            sector = read_sector(self.image, self.fs_start + 1 + ino)
            used, typ, parent, size = struct.unpack_from("<BBHI", sector, 0)
            blocks = list(struct.unpack_from("<8H", sector, 8))
            indirect = struct.unpack_from("<H", sector, 24)[0]
            double_indirect = struct.unpack_from("<H", sector, 26)[0]
            self.inodes.append({
                "ino": ino,
                "used": used,
                "type": typ,
                "parent": parent,
                "size": size,
                "blocks": blocks,
                "indirect": indirect,
                "double_indirect": double_indirect,
            })

        bitmap_off = self.bitmap_offset()
        bitmap_size = self.expected_bitmap_sectors * SECTOR_SIZE
        self.bitmap = list(self.image[bitmap_off:bitmap_off + bitmap_size]
                           [:self.expected_blocks])
        for i, value in enumerate(self.bitmap):
            if value not in (0, 1):
                if not allow_invalid_bitmap:
                    fail(f"bitmap block {i} has invalid value {value}")

    def data_sector(self, block_index):
        if block_index < 0 or block_index >= self.expected_blocks:
            fail(f"data block {block_index} is outside range")
        return read_sector(self.image, self.expected_data_lba + block_index)

    def note_block_ref(self, block_index, owner):
        if block_index < 0 or block_index >= self.expected_blocks:
            fail(f"{owner} references data block {block_index}, outside range")
        self.block_refs.setdefault(block_index, []).append(owner)

    def inode_blocks(self, inode, include_indirect_table=True, note_refs=True):
        result = []
        for logical, raw in enumerate(inode["blocks"]):
            if raw:
                block_index = raw - 1
                if note_refs:
                    self.note_block_ref(block_index, f"inode {inode['ino']} direct[{logical}]")
                result.append((logical, block_index))
        if inode["indirect"]:
            table_index = inode["indirect"] - 1
            if include_indirect_table and note_refs:
                self.note_block_ref(table_index, f"inode {inode['ino']} indirect-table")
            table = self.data_sector(table_index)
            for i in range(SECTOR_SIZE // 2):
                raw = struct.unpack_from("<H", table, i * 2)[0]
                if raw:
                    block_index = raw - 1
                    logical = MINIFS_DIRECT + i
                    if note_refs:
                        self.note_block_ref(block_index, f"inode {inode['ino']} indirect[{i}]")
                    result.append((logical, block_index))
        if inode["double_indirect"]:
            outer_index = inode["double_indirect"] - 1
            if include_indirect_table and note_refs:
                self.note_block_ref(
                    outer_index, f"inode {inode['ino']} double-indirect-table"
                )
            outer = self.data_sector(outer_index)
            for i in range(SECTOR_SIZE // 2):
                inner_raw = struct.unpack_from("<H", outer, i * 2)[0]
                if not inner_raw:
                    continue
                inner_index = inner_raw - 1
                if include_indirect_table and note_refs:
                    self.note_block_ref(
                        inner_index,
                        f"inode {inode['ino']} double-indirect[{i}]-table",
                    )
                inner = self.data_sector(inner_index)
                for j in range(SECTOR_SIZE // 2):
                    raw = struct.unpack_from("<H", inner, j * 2)[0]
                    if raw:
                        block_index = raw - 1
                        logical = (MINIFS_DIRECT + SECTOR_SIZE // 2 +
                                   i * (SECTOR_SIZE // 2) + j)
                        if note_refs:
                            self.note_block_ref(
                                block_index,
                                f"inode {inode['ino']} double-indirect[{i}][{j}]",
                            )
                        result.append((logical, block_index))
        return result

    def read_file_bytes(self, inode):
        out = bytearray()
        block_map = dict(self.inode_blocks(inode, include_indirect_table=False, note_refs=False))
        needed = (inode["size"] + SECTOR_SIZE - 1) // SECTOR_SIZE
        for logical in range(needed):
            if logical not in block_map:
                fail(f"inode {inode['ino']} is missing logical block {logical}")
            out.extend(self.data_sector(block_map[logical]))
        return bytes(out[:inode["size"]])

    def read_dir_entries(self, inode):
        if inode["size"] % MINIFS_DIRENT_SIZE != 0:
            fail(f"directory inode {inode['ino']} size is not dirent-aligned")
        data = self.read_file_bytes(inode)
        entries = []
        seen_names = set()
        for off in range(0, len(data), MINIFS_DIRENT_SIZE):
            ino, typ = struct.unpack_from("<HB", data, off)
            if ino == 0:
                continue
            if ino >= MINIFS_INODES:
                fail(f"directory inode {inode['ino']} entry points outside inode table")
            target = self.inodes[ino]
            if not target["used"]:
                fail(f"directory inode {inode['ino']} entry points to free inode {ino}")
            if typ != target["type"]:
                fail(f"directory inode {inode['ino']} entry type mismatch for inode {ino}")
            name_raw = data[off + 3:off + 3 + MINIFS_NAME_LEN]
            if 0 not in name_raw:
                fail(f"directory inode {inode['ino']} has unterminated entry name")
            name = c_name(name_raw)
            if not name or "/" in name or len(name) >= MINIFS_NAME_LEN:
                fail(f"directory inode {inode['ino']} has invalid entry name {name!r}")
            if name in seen_names:
                fail(f"directory inode {inode['ino']} has duplicate name {name}")
            seen_names.add(name)
            if target["parent"] != inode["ino"]:
                fail(f"inode {ino} parent is {target['parent']}, expected {inode['ino']}")
            entries.append((name, ino, typ))
        self.dir_edges[inode["ino"]] = entries
        return entries

    def check_inodes(self, repair=False):
        root = self.inodes[MINIFS_ROOT_INO]
        if not root["used"] or root["type"] != MINIFS_DIR or root["parent"] != MINIFS_ROOT_INO:
            fail("root inode is not a self-parented directory")
        for inode in self.inodes:
            ino = inode["ino"]
            if inode["used"] not in (0, 1):
                fail(f"inode {ino} has invalid used flag {inode['used']}")
            if not inode["used"]:
                if (inode["type"] or inode["parent"] or inode["size"] or
                        inode["indirect"] or inode["double_indirect"] or
                        any(inode["blocks"])):
                    if repair:
                        self.zero_inode_sector(ino, f"zeroed stale free inode {ino}")
                        continue
                    fail(f"free inode {ino} contains stale metadata")
                continue
            if inode["type"] not in (MINIFS_DIR, MINIFS_FILE):
                fail(f"inode {ino} has invalid type {inode['type']}")
            if ino != MINIFS_ROOT_INO:
                if inode["parent"] >= MINIFS_INODES or not self.inodes[inode["parent"]]["used"]:
                    fail(f"inode {ino} has invalid parent {inode['parent']}")
                if self.inodes[inode["parent"]]["type"] != MINIFS_DIR:
                    fail(f"inode {ino} parent {inode['parent']} is not a directory")
            max_file_blocks = self.expected_blocks
            if inode["size"] > max_file_blocks * SECTOR_SIZE:
                fail(f"inode {ino} exceeds max file size")

    def check_blocks(self, repair=False):
        invalid_bitmap = [i for i, value in enumerate(self.bitmap) if value not in (0, 1)]
        if invalid_bitmap and not repair:
            fail(f"bitmap block {invalid_bitmap[0]} has invalid value {self.bitmap[invalid_bitmap[0]]}")

        for inode in self.inodes:
            if inode["used"]:
                self.inode_blocks(inode)

        duplicates = {b: owners for b, owners in self.block_refs.items() if len(owners) > 1}
        if duplicates:
            first = next(iter(duplicates.items()))
            fail(f"data block {first[0]} is referenced multiple times: {', '.join(first[1])}")

        referenced = set(self.block_refs)
        marked = {i for i, value in enumerate(self.bitmap) if value}
        if repair:
            changed = False
            for i in range(self.expected_blocks):
                expected = 1 if i in referenced else 0
                if self.bitmap[i] != expected:
                    self.bitmap[i] = expected
                    changed = True
            if changed:
                self.write_bitmap("rebuilt bitmap from inode block references")
            return

        missing = sorted(referenced - marked)
        leaked = sorted(marked - referenced)
        if missing:
            fail(f"referenced block {missing[0]} is not marked used in bitmap")
        if leaked:
            fail(f"bitmap marks unreferenced block {leaked[0]} as used")

    def walk_dirs(self, ino=MINIFS_ROOT_INO, stack=None):
        if stack is None:
            stack = set()
        if ino in stack:
            fail(f"directory cycle reaches inode {ino}")
        stack.add(ino)
        self.reachable.add(ino)
        inode = self.inodes[ino]
        entries = self.read_dir_entries(inode)
        for name, child, typ in entries:
            parent_path = self.paths[ino]
            self.paths[child] = (parent_path.rstrip("/") + "/" + name) if parent_path != "/" else "/" + name
            if typ == MINIFS_DIR:
                self.walk_dirs(child, stack)
            else:
                self.reachable.add(child)
        stack.remove(ino)

    def check_reachability(self):
        self.walk_dirs()
        used = {inode["ino"] for inode in self.inodes if inode["used"]}
        unreachable = sorted(used - self.reachable)
        if unreachable:
            fail(f"used inode {unreachable[0]} is not reachable from root")

    def check(self, repair=False):
        self.reset_walk_state()
        self.load(allow_invalid_bitmap=repair)
        self.check_inodes(repair=repair)
        self.check_blocks(repair=repair)
        self.check_reachability()
        used_inodes = [inode for inode in self.inodes if inode["used"]]
        dirs = [inode for inode in used_inodes if inode["type"] == MINIFS_DIR]
        files = [inode for inode in used_inodes if inode["type"] == MINIFS_FILE]
        used_blocks = sum(1 for value in self.bitmap if value)
        ok(f"minifs super: inodes={self.super['inode_count']} blocks={self.super['block_count']} data_lba={self.super['data_lba']}")
        ok(f"minifs usage: {len(used_inodes)} inodes ({len(dirs)} dirs, {len(files)} files), {used_blocks}/{self.expected_blocks} blocks")
        if repair:
            if self.repairs:
                for repair_msg in self.repairs:
                    print(f"[repair] {repair_msg}")
            else:
                print("[repair] no changes needed")

    def list_tree(self):
        rows = []
        for ino, path in sorted(self.paths.items(), key=lambda item: item[1]):
            inode = self.inodes[ino]
            typ = "dir " if inode["type"] == MINIFS_DIR else "file"
            rows.append((path, typ, inode["size"], ino))
        print("type   size    ino  path")
        print("-----  ------  ---  ----")
        for path, typ, size, ino in rows:
            print(f"{typ:<5}  {size:>6}  {ino:>3}  {path}")


def main():
    parser = argparse.ArgumentParser(description="Check a BuzzOS minifs image")
    parser.add_argument("--image", default="build/buzzos.img", help="raw disk image to inspect")
    parser.add_argument("--fs-start", type=int, default=parse_make_int("FS_START_SECTOR", 512))
    parser.add_argument("--fs-sectors", type=int, default=parse_make_int("FS_SECTORS", 512))
    parser.add_argument("--allow-empty", action="store_true", help="treat an unformatted all-zero /fs region as success")
    parser.add_argument("--list", action="store_true", help="print the filesystem tree after checks")
    parser.add_argument("--repair", action="store_true", help="repair safe metadata drift in a copy or in-place")
    parser.add_argument("--out", help="write repaired image to this path; required with --repair unless --in-place is set")
    parser.add_argument("--in-place", action="store_true", help="with --repair, overwrite the input image")
    args = parser.parse_args()

    path = ROOT / args.image if not Path(args.image).is_absolute() else Path(args.image)
    if not path.exists():
        print(f"[fail] image does not exist: {path}", file=sys.stderr)
        return 1
    if args.repair and not args.in_place and not args.out:
        print("[fail] --repair requires --out or --in-place", file=sys.stderr)
        return 1
    if args.out and not args.repair:
        print("[fail] --out is only valid with --repair", file=sys.stderr)
        return 1

    image = bytearray(path.read_bytes()) if args.repair else path.read_bytes()
    fs = MiniFsImage(image, args.fs_start, args.fs_sectors)
    try:
        if fs.is_empty_region():
            message = f"{path} has an unformatted all-zero /fs region"
            if args.allow_empty:
                warn(message)
                return 0
            fail(message)
        fs.check(repair=args.repair)
        if args.list:
            fs.list_tree()
        if args.repair:
            out = path if args.in_place else (ROOT / args.out if not Path(args.out).is_absolute() else Path(args.out))
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(bytes(image))
            print(f"Wrote repaired image: {out}")
            verify = MiniFsImage(out.read_bytes(), args.fs_start, args.fs_sectors)
            verify.check()
        print("Minifs check passed.")
        return 0
    except FsError as exc:
        print(f"[fail] {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
