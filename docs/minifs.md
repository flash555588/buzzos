# Minifs Image Checks

BuzzOS stores the persistent `/fs` filesystem inside the raw disk image. The
host-side checker in `tools/check_minifs.py` inspects that filesystem without
booting QEMU.

## Layout

The default image layout is:

```text
LBA 67584          superblock
LBA 67585..69632   inode table, one inode per sector
LBA 69633..69756   data block bitmap
LBA 69757..133119  data blocks
```

The current format uses a 65536-sector (32 MiB) raw `/fs` partition. It has
2048 inodes, 124 bitmap sectors, and 63363 data blocks (~30.9 MiB usable).
Each inode stores eight direct block references plus single- and double-indirect
block tables. Directory entries are 32-byte records containing an inode number,
type, and 24-byte name. Images made by the older 128-inode formats are migrated
in place on mount. Expanding `MINIFS_SECTORS` / `FS_SECTORS` reformats on the
next mount when the superblock block count no longer matches.

## Commands

Check the persistent filesystem in the main image:

```sh
make fs-check
```

List the tree after checking it:

```sh
make fs-ls
```

Write a conservatively repaired copy of the current image:

```sh
make fs-repair
```

By default this writes `build/buzzos-repaired.img` and does not overwrite the
current image. Override the input or output image when needed:

```sh
make fs-repair FS_IMAGE=build/buzzos-test.img FS_REPAIR_IMAGE=build/buzzos-test-fixed.img
```

Check a specific image:

```sh
python tools/check_minifs.py --image build/buzzos-test.img --list
```

Run the negative corruption fixtures against the post-smoke image:

```sh
make fs-check-negative
```

That target mutates a copy of `build/buzzos-test.img` in memory and confirms the
checker rejects bad superblocks, invalid bitmap bytes, missing bitmap marks,
leaked bitmap marks, root inode type corruption, out-of-range block references,
duplicate block references, stale free inode metadata, and bad parent links.

Run the repair-mode fixtures against the post-smoke image:

```sh
make fs-check-repair
```

Repair mode is intentionally conservative. It only fixes metadata drift that can
be derived from otherwise valid inode/block references:

- Zero stale metadata in free inode sectors.
- Rebuild the data-block bitmap from inode references.
- Normalize invalid bitmap bytes while rebuilding that bitmap.

Repair writes to a copy unless `--in-place` is explicit:

```sh
make fs-repair
python tools/check_minifs.py --image build/buzzos-test.img --repair --out build/buzzos-test-fixed.img
```

The smoke test boots a disposable copy of the image and leaves it at
`build/buzzos-test.img`. `make verify` now checks that post-boot test image, so
it verifies `/fs` after real kernel formatting, app seeding, file creation, and
file deletion.

Inside BuzzOS, the shell exposes a small read-only view of the same metadata:

```text
fsinfo
cat /proc/fs
fsstat
```

`fsinfo` reads `/proc/fs` and prints the mount, driver, status, image location,
inode/block counters, maximum file size, and the matching host-side check and
repair commands. `fsstat` remains the compact syscall-backed counter view. Both
in-OS commands are intentionally smaller than the host checker; they report
current counters, while `tools/check_minifs.py` performs cross-reference
validation.

## What It Validates

- Superblock magic, inode count, block count, and data LBA.
- Inode used/type/parent fields.
- File and directory block references.
- Duplicate, out-of-range, leaked, or unmarked data blocks.
- Directory entry alignment, names, duplicate names, types, and parent links.
- Reachability of every used inode from the root directory.

By default the checker is read-only. With `--repair`, it refuses unsafe
corruption such as bad superblocks, broken roots, duplicate blocks, out-of-range
block references, bad parents, and directory graph problems. The negative and
repair fixture runners are read-only unless `--out-dir` is explicitly passed for
debugging corrupted image copies.
