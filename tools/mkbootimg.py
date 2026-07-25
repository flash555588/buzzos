#!/usr/bin/env python3
import argparse
import math
import struct
import subprocess
from pathlib import Path

SECTOR_SIZE = 512


def align_up(value, align):
    return (value + align - 1) // align * align


def read_old_fs(out_path: Path, fs_start: int, fs_sectors: int):
    if not out_path.exists():
        return None
    data = out_path.read_bytes()
    offset = fs_start * SECTOR_SIZE
    length = fs_sectors * SECTOR_SIZE
    if offset >= len(data):
        return None
    # Preserve an older, smaller filesystem when the raw partition grows.
    # The kernel migrates its metadata/data layout on the next mount.
    old = data[offset:min(offset + length, len(data))]
    return old + bytes(length - len(old))


def lba_to_chs(lba: int):
    if lba > 1024 * 255 * 63 - 1:
        return bytes((0xFE, 0xFF, 0xFF))
    c = lba // (255 * 63)
    h = (lba // 63) % 255
    s = (lba % 63) + 1
    return bytes((h & 0xFF, ((c >> 2) & 0xC0) | (s & 0x3F), c & 0xFF))


def fat_short_checksum(short_name: bytes) -> int:
    value = 0
    for b in short_name:
        value = (((value & 1) << 7) | ((value & 0xFE) >> 1)) + b
        value &= 0xFF
    return value


def make_lfn_entries(name: str, short_name: bytes):
    chars = [ord(c) for c in name]
    chunks = [chars[i:i + 13] for i in range(0, len(chars), 13)]
    checksum = fat_short_checksum(short_name)
    entries = []
    for chunk_idx in range(len(chunks) - 1, -1, -1):
        chunk = list(chunks[chunk_idx])
        order = chunk_idx + 1
        if chunk_idx == len(chunks) - 1:
            order |= 0x40
        if len(chunk) < 13:
            chunk.append(0x0000)
        while len(chunk) < 13:
            chunk.append(0xFFFF)
        entry = bytearray(32)
        entry[0] = order
        for off, val in zip((1, 3, 5, 7, 9), chunk[0:5]):
            struct.pack_into("<H", entry, off, val)
        entry[11] = 0x0F
        entry[12] = 0
        entry[13] = checksum
        for off, val in zip((14, 16, 18, 20, 22, 24), chunk[5:11]):
            struct.pack_into("<H", entry, off, val)
        struct.pack_into("<H", entry, 26, 0)
        for off, val in zip((28, 30), chunk[11:13]):
            struct.pack_into("<H", entry, off, val)
        entries.append(bytes(entry))
    return entries


def build_root_dir_entries(files):
    entries = []
    for file in files:
        entries.extend(make_lfn_entries(file["name"], file["short"]))
        short = bytearray(32)
        short[0:11] = file["short"]
        short[11] = 0x20
        struct.pack_into("<H", short, 26, file["first_cluster"])
        struct.pack_into("<I", short, 28, len(file["data"]))
        entries.append(bytes(short))
    return b"".join(entries)


def choose_spc(total_sectors: int) -> int:
    for spc in (1, 2, 4, 8, 16, 32, 64):
        root_dir_sectors = 32
        reserved = 1
        fats = 2
        fat_sectors = 1
        for _ in range(8):
            data_sectors = total_sectors - reserved - fats * fat_sectors - root_dir_sectors
            clusters = data_sectors // spc
            fat_sectors = math.ceil((clusters + 2) * 2 / SECTOR_SIZE)
        if 4085 <= clusters < 65525:
            return spc
    raise RuntimeError("could not find FAT16 sectors-per-cluster")


def build_fat16(partition_start: int, total_sectors: int, files):
    spc = choose_spc(total_sectors)
    reserved = 1
    fats = 2
    root_entries = 512
    root_dir_sectors = (root_entries * 32 + SECTOR_SIZE - 1) // SECTOR_SIZE
    fat_sectors = 1
    for _ in range(8):
        data_sectors = total_sectors - reserved - fats * fat_sectors - root_dir_sectors
        clusters = data_sectors // spc
        fat_sectors = math.ceil((clusters + 2) * 2 / SECTOR_SIZE)
    data_start_sector = reserved + fats * fat_sectors + root_dir_sectors
    cluster_size = spc * SECTOR_SIZE

    next_cluster = 2
    fat = [0xFFF8, 0xFFFF]
    while len(fat) < clusters + 2:
        fat.append(0x0000)

    for file in files:
        data = file["data"]
        if not data:
            file["first_cluster"] = 0
            continue
        needed = math.ceil(len(data) / cluster_size)
        first = next_cluster
        file["first_cluster"] = first
        for i in range(needed):
            cluster = next_cluster + i
            fat[cluster] = 0xFFFF if i == needed - 1 else cluster + 1
        next_cluster += needed

    if next_cluster > clusters + 2:
        raise RuntimeError("boot partition too small for files")

    partition = bytearray(total_sectors * SECTOR_SIZE)
    boot = memoryview(partition)[:SECTOR_SIZE]
    boot[0:3] = b"\xEB\x3C\x90"
    boot[3:11] = b"BUZZFAT "
    struct.pack_into("<H", boot, 11, SECTOR_SIZE)
    boot[13] = spc
    struct.pack_into("<H", boot, 14, reserved)
    boot[16] = fats
    struct.pack_into("<H", boot, 17, root_entries)
    struct.pack_into("<H", boot, 19, total_sectors if total_sectors < 0x10000 else 0)
    boot[21] = 0xF8
    struct.pack_into("<H", boot, 22, fat_sectors)
    struct.pack_into("<H", boot, 24, 63)
    struct.pack_into("<H", boot, 26, 255)
    struct.pack_into("<I", boot, 28, partition_start)
    struct.pack_into("<I", boot, 32, total_sectors if total_sectors >= 0x10000 else 0)
    boot[36] = 0x80
    boot[38] = 0x29
    struct.pack_into("<I", boot, 39, 0x42555A5A)
    boot[43:54] = b"BUZZOSBOOT "
    boot[54:62] = b"FAT16   "
    boot[510:512] = b"\x55\xAA"

    fat_bytes = bytearray(fat_sectors * SECTOR_SIZE)
    for i, value in enumerate(fat):
        struct.pack_into("<H", fat_bytes, i * 2, value)
    for fat_idx in range(fats):
        offset = (reserved + fat_idx * fat_sectors) * SECTOR_SIZE
        partition[offset:offset + len(fat_bytes)] = fat_bytes

    root_entries_bytes = build_root_dir_entries(files)
    root_dir = bytearray(root_dir_sectors * SECTOR_SIZE)
    root_dir[:len(root_entries_bytes)] = root_entries_bytes
    root_offset = (reserved + fats * fat_sectors) * SECTOR_SIZE
    partition[root_offset:root_offset + len(root_dir)] = root_dir

    for file in files:
        data = file["data"]
        if not data:
            continue
        cluster = file["first_cluster"]
        pos = 0
        while cluster >= 2 and cluster < 0xFFF8:
            sector = data_start_sector + (cluster - 2) * spc
            offset = sector * SECTOR_SIZE
            chunk = data[pos:pos + cluster_size]
            partition[offset:offset + len(chunk)] = chunk
            pos += len(chunk)
            cluster = fat[cluster]

    return bytes(partition)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--limine-dir", required=True)
    ap.add_argument("--boot-partition-start", type=int, required=True)
    ap.add_argument("--boot-partition-sectors", type=int, required=True)
    ap.add_argument("--fs-start", type=int, required=True)
    ap.add_argument("--fs-sectors", type=int, required=True)
    ap.add_argument("--reset-fs", action="store_true")
    args = ap.parse_args()

    kernel_path = Path(args.kernel).resolve()
    out_path = Path(args.out).resolve()
    limine_dir = Path(args.limine_dir).resolve()
    limine_tool = limine_dir / "limine-tool-windows-x86" / "limine.exe"
    limine_bios_sys = limine_dir / "limine-bios.sys"

    files = [
        {
            "name": "kernel.elf",
            "short": b"KERNEL  ELF",
            "data": kernel_path.read_bytes(),
        },
        {
            "name": "limine.conf",
            "short": b"LIMINE~1CFG",
            "data": (
                "timeout: 0\n"
                "serial: yes\n"
                "verbose: yes\n"
                "/BuzzOS\n"
                "    protocol: multiboot2\n"
                "    kernel_path: boot():/kernel.elf\n"
                "    resolution: 1600x900\n"
            ).encode("ascii"),
        },
        {
            "name": "limine-bios.sys",
            "short": b"LIMINE~1SYS",
            "data": limine_bios_sys.read_bytes(),
        },
    ]

    total_sectors = args.fs_start + args.fs_sectors
    image = bytearray(total_sectors * SECTOR_SIZE)

    # MBR with boot FAT16 partition + raw /fs partition.
    part1 = struct.pack(
        "<B3sB3sII",
        0x80,
        lba_to_chs(args.boot_partition_start),
        0x06,
        lba_to_chs(args.boot_partition_start + args.boot_partition_sectors - 1),
        args.boot_partition_start,
        args.boot_partition_sectors,
    )
    part2 = struct.pack(
        "<B3sB3sII",
        0x00,
        lba_to_chs(args.fs_start),
        0x83,
        lba_to_chs(args.fs_start + args.fs_sectors - 1),
        args.fs_start,
        args.fs_sectors,
    )
    image[446:446 + 16] = part1
    image[462:462 + 16] = part2
    image[510:512] = b"\x55\xAA"

    boot_partition = build_fat16(
        args.boot_partition_start,
        args.boot_partition_sectors,
        files,
    )
    boot_offset = args.boot_partition_start * SECTOR_SIZE
    image[boot_offset:boot_offset + len(boot_partition)] = boot_partition

    fs_bytes = None if args.reset_fs else read_old_fs(out_path, args.fs_start, args.fs_sectors)
    if fs_bytes is None:
        fs_bytes = bytes(args.fs_sectors * SECTOR_SIZE)
    fs_offset = args.fs_start * SECTOR_SIZE
    image[fs_offset:fs_offset + len(fs_bytes)] = fs_bytes

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(image)

    subprocess.run(
        [str(limine_tool), "bios-install", str(out_path)],
        check=True,
    )
    print(f"Wrote {out_path} ({len(image)} bytes)")


if __name__ == "__main__":
    main()
