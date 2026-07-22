#!/usr/bin/env python3
"""Install a Doom IWAD directly into a BuzzOS MiniFS disk image."""

import argparse
import struct
from pathlib import Path

from check_minifs import (
    MiniFsImage, MINIFS_DIRECT, MINIFS_DIRENT_SIZE, MINIFS_DIR, MINIFS_FILE,
    MINIFS_INODES, MINIFS_MAGIC, MINIFS_NAME_LEN, MINIFS_ROOT_INO, SECTOR_SIZE,
    parse_make_int,
)


class MiniFsWriter:
    def __init__(self, data, fs_start, fs_sectors):
        self.data = data
        self.fs = MiniFsImage(data, fs_start, fs_sectors)
        if self.fs.is_empty_region():
            self.format()
        self.fs.load()
        self.fs.check_inodes()
        self.fs.check_blocks()
        self.fs.walk_dirs()

    def format(self):
        fs = self.fs
        start = fs.fs_start * SECTOR_SIZE
        end = (fs.fs_start + fs.fs_sectors) * SECTOR_SIZE
        self.data[start:end] = bytes(end - start)
        struct.pack_into("<IIII", self.data, start, MINIFS_MAGIC, MINIFS_INODES,
                         fs.expected_blocks, fs.expected_data_lba)
        off = fs.inode_offset(MINIFS_ROOT_INO)
        struct.pack_into("<BBHI", self.data, off, 1, MINIFS_DIR,
                         MINIFS_ROOT_INO, 0)

    def write_bitmap(self):
        off = self.fs.bitmap_offset()
        for i, used in enumerate(self.fs.bitmap[:self.fs.expected_blocks]):
            self.data[off + i] = 1 if used else 0

    def block_offset(self, block):
        return (self.fs.expected_data_lba + block) * SECTOR_SIZE

    def alloc_block(self):
        for block, used in enumerate(self.fs.bitmap[:self.fs.expected_blocks]):
            if not used:
                self.fs.bitmap[block] = 1
                off = self.block_offset(block)
                self.data[off:off + SECTOR_SIZE] = bytes(SECTOR_SIZE)
                return block
        raise RuntimeError("MiniFS has no free data blocks")

    def alloc_inode(self, typ, parent):
        for ino in range(2, MINIFS_INODES):
            if not self.fs.inodes[ino]["used"]:
                inode = {
                    "ino": ino, "used": 1, "type": typ, "parent": parent,
                    "size": 0, "blocks": [0] * MINIFS_DIRECT,
                    "indirect": 0, "double_indirect": 0,
                }
                self.fs.inodes[ino] = inode
                self.write_inode(inode)
                return inode
        raise RuntimeError("MiniFS has no free inodes; reset the image or clear the browser cache")

    def write_inode(self, inode):
        off = self.fs.inode_offset(inode["ino"])
        self.data[off:off + SECTOR_SIZE] = bytes(SECTOR_SIZE)
        struct.pack_into("<BBHI", self.data, off, inode["used"], inode["type"],
                         inode["parent"], inode["size"])
        struct.pack_into("<8H", self.data, off + 8, *inode["blocks"])
        struct.pack_into("<HH", self.data, off + 24, inode["indirect"],
                         inode["double_indirect"])

    def old_blocks(self, inode):
        blocks = [raw - 1 for raw in inode["blocks"] if raw]
        if inode["indirect"]:
            table = inode["indirect"] - 1
            blocks.append(table)
            off = self.block_offset(table)
            blocks.extend(raw - 1 for (raw,) in struct.iter_unpack(
                "<H", self.data[off:off + SECTOR_SIZE]) if raw)
        if inode["double_indirect"]:
            outer = inode["double_indirect"] - 1
            blocks.append(outer)
            outer_off = self.block_offset(outer)
            for (inner_raw,) in struct.iter_unpack(
                    "<H", self.data[outer_off:outer_off + SECTOR_SIZE]):
                if not inner_raw:
                    continue
                inner = inner_raw - 1
                blocks.append(inner)
                inner_off = self.block_offset(inner)
                blocks.extend(raw - 1 for (raw,) in struct.iter_unpack(
                    "<H", self.data[inner_off:inner_off + SECTOR_SIZE]) if raw)
        return blocks

    def set_contents(self, inode, payload):
        old = self.old_blocks(inode)
        count = (len(payload) + SECTOR_SIZE - 1) // SECTOR_SIZE
        data_blocks = [self.alloc_block() for _ in range(count)]
        direct = data_blocks[:MINIFS_DIRECT]
        remaining = data_blocks[MINIFS_DIRECT:]
        indirect_raw = 0
        double_raw = 0
        if remaining:
            table = self.alloc_block()
            indirect_raw = table + 1
            off = self.block_offset(table)
            first = remaining[:SECTOR_SIZE // 2]
            for i, block in enumerate(first):
                struct.pack_into("<H", self.data, off + i * 2, block + 1)
            remaining = remaining[SECTOR_SIZE // 2:]
        if remaining:
            outer = self.alloc_block()
            double_raw = outer + 1
            outer_off = self.block_offset(outer)
            for group_index in range(0, len(remaining), SECTOR_SIZE // 2):
                inner = self.alloc_block()
                slot = group_index // (SECTOR_SIZE // 2)
                if slot >= SECTOR_SIZE // 2:
                    raise RuntimeError("file exceeds MiniFS double-indirect capacity")
                struct.pack_into("<H", self.data, outer_off + slot * 2, inner + 1)
                inner_off = self.block_offset(inner)
                group = remaining[group_index:group_index + SECTOR_SIZE // 2]
                for i, block in enumerate(group):
                    struct.pack_into("<H", self.data, inner_off + i * 2, block + 1)
        for i, block in enumerate(data_blocks):
            off = self.block_offset(block)
            chunk = payload[i * SECTOR_SIZE:(i + 1) * SECTOR_SIZE]
            self.data[off:off + len(chunk)] = chunk
        for block in old:
            self.fs.bitmap[block] = 0
        inode["size"] = len(payload)
        inode["blocks"] = [b + 1 for b in direct] + [0] * (MINIFS_DIRECT - len(direct))
        inode["indirect"] = indirect_raw
        inode["double_indirect"] = double_raw
        self.write_inode(inode)

    def child(self, parent, name):
        for entry_name, ino, _ in self.fs.read_dir_entries(parent):
            if entry_name == name:
                return self.fs.inodes[ino]
        return None

    def add_child(self, parent, name, typ):
        try:
            name_bytes = name.encode("utf-8")
        except UnicodeEncodeError as exc:
            raise RuntimeError(f"invalid MiniFS name: {name}") from exc
        # MINIFS_NAME_LEN includes the trailing NUL; leaf names are UTF-8 bytes.
        if not name_bytes or len(name_bytes) >= MINIFS_NAME_LEN:
            raise RuntimeError(
                f"invalid MiniFS name (need 1..{MINIFS_NAME_LEN - 1} UTF-8 bytes): {name}"
            )
        existing = self.child(parent, name)
        if existing:
            if existing["type"] != typ:
                raise RuntimeError(f"{name} exists with the wrong type")
            return existing
        inode = self.alloc_inode(typ, parent["ino"])
        current = self.fs.read_file_bytes(parent)
        entry = struct.pack("<HB24s5x", inode["ino"], typ,
                            name_bytes + bytes(MINIFS_NAME_LEN - len(name_bytes)))
        assert len(entry) == MINIFS_DIRENT_SIZE
        self.set_contents(parent, current + entry)
        return inode

    def install(self, wad):
        root = self.fs.inodes[MINIFS_ROOT_INO]
        games = self.add_child(root, "games", MINIFS_DIR)
        doom = self.add_child(games, "doom", MINIFS_DIR)
        target = self.add_child(doom, "doom1.wad", MINIFS_FILE)
        self.set_contents(target, wad)
        self.write_bitmap()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", default="build/buzzos.img")
    parser.add_argument("--wad", required=True)
    parser.add_argument("--fs-start", type=int, default=parse_make_int("FS_START", 67584))
    parser.add_argument("--fs-sectors", type=int, default=parse_make_int("FS_SECTORS", 65536))
    args = parser.parse_args()
    image_path = Path(args.image)
    wad_path = Path(args.wad)
    wad = wad_path.read_bytes()
    if len(wad) < 12 or wad[:4] not in (b"IWAD", b"PWAD"):
        raise SystemExit(f"{wad_path} is not a Doom WAD")
    data = bytearray(image_path.read_bytes())
    writer = MiniFsWriter(data, args.fs_start, args.fs_sectors)
    writer.install(wad)
    image_path.write_bytes(data)
    print(f"Installed {wad_path} ({len(wad)} bytes) as /fs/games/doom/doom1.wad")


if __name__ == "__main__":
    main()
