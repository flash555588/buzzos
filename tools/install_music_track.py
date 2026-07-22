#!/usr/bin/env python3
"""Install an MP3/WAV track into a BuzzOS MiniFS image under /fs/music/."""

import argparse
from pathlib import Path

from check_minifs import MINIFS_DIR, MINIFS_FILE, MINIFS_NAME_LEN, MINIFS_ROOT_INO, parse_make_int
from install_doom_wad import MiniFsWriter


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", default="build/buzzos.img")
    parser.add_argument("--track", required=True, help="host path to .mp3 or .wav")
    parser.add_argument(
        "--name",
        default="",
        help="destination filename under /fs/music (UTF-8, max 23 bytes). "
             "Default: basename of --track, truncated if needed.",
    )
    parser.add_argument("--fs-start", type=int, default=parse_make_int("FS_START_SECTOR", 67584))
    parser.add_argument("--fs-sectors", type=int, default=parse_make_int("FS_SECTORS", 65536))
    args = parser.parse_args()

    image_path = Path(args.image)
    track_path = Path(args.track)
    if not image_path.is_file():
        raise SystemExit(f"image not found: {image_path} (run make first)")
    if not track_path.is_file():
        raise SystemExit(f"track not found: {track_path}")

    target_name = args.name.strip() if args.name else track_path.name
    if Path(target_name).name != target_name or not target_name:
        raise SystemExit("--name must be a plain filename")
    name_bytes = target_name.encode("utf-8")
    if len(name_bytes) >= MINIFS_NAME_LEN:
        raise SystemExit(
            f"--name is {len(name_bytes)} UTF-8 bytes; MiniFS limit is "
            f"{MINIFS_NAME_LEN - 1} (NUL-terminated). "
            f"Try a shorter name such as 青花瓷.mp3"
        )

    payload = track_path.read_bytes()
    if len(payload) < 4:
        raise SystemExit(f"{track_path} is empty or too small")

    data = bytearray(image_path.read_bytes())
    writer = MiniFsWriter(data, args.fs_start, args.fs_sectors)
    root = writer.fs.inodes[MINIFS_ROOT_INO]
    music = writer.add_child(root, "music", MINIFS_DIR)
    target = writer.add_child(music, target_name, MINIFS_FILE)
    writer.set_contents(target, payload)
    writer.write_bitmap()
    image_path.write_bytes(data)
    print(
        f"Installed {track_path} ({len(payload)} bytes) as /fs/music/{target_name}"
    )


if __name__ == "__main__":
    main()
