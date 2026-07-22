#!/usr/bin/env python3
"""Install a user-supplied Game Boy ROM into a BuzzOS MiniFS image."""
import argparse
from pathlib import Path

from check_minifs import MINIFS_DIR, MINIFS_FILE, MINIFS_ROOT_INO, parse_make_int
from install_doom_wad import MiniFsWriter


def valid_rom(data):
    if len(data) < 0x150 or len(data) % 0x4000:
        return False
    checksum = 0
    for value in data[0x134:0x14D]:
        checksum = (checksum - value - 1) & 0xFF
    return checksum == data[0x14D]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", default="build/buzzos.img")
    parser.add_argument("--rom", required=True)
    parser.add_argument("--name", default="game.gb",
                        help="destination filename under /fs/games/gameboy")
    parser.add_argument("--fs-start", type=int, default=parse_make_int("FS_START", 67584))
    parser.add_argument("--fs-sectors", type=int, default=parse_make_int("FS_SECTORS", 32768))
    args = parser.parse_args()
    image_path, rom_path = Path(args.image), Path(args.rom)
    target_name = args.name
    if Path(target_name).name != target_name or not target_name:
        raise SystemExit("--name must be a plain filename")
    try:
        target_name.encode("ascii")
    except UnicodeEncodeError:
        raise SystemExit("--name must contain ASCII characters only")
    if not rom_path.is_file():
        raise SystemExit("pass a Game Boy ROM, for example: make gameboy-install ROM=C:\\games\\tetris.gb")
    rom = rom_path.read_bytes()
    if not valid_rom(rom):
        raise SystemExit(f"{rom_path} is not a valid header-checksummed Game Boy ROM")
    data = bytearray(image_path.read_bytes())
    writer = MiniFsWriter(data, args.fs_start, args.fs_sectors)
    root = writer.fs.inodes[MINIFS_ROOT_INO]
    games = writer.add_child(root, "games", MINIFS_DIR)
    gameboy = writer.add_child(games, "gameboy", MINIFS_DIR)
    target = writer.add_child(gameboy, target_name, MINIFS_FILE)
    writer.set_contents(target, rom)
    writer.write_bitmap()
    image_path.write_bytes(data)
    print(f"Installed {rom_path} ({len(rom)} bytes) as /fs/games/gameboy/{target_name}")


if __name__ == "__main__":
    main()
