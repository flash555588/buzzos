# Vendored Limine (BIOS)

Source: [Limine v12.5.2 binary release](https://github.com/limine-bootloader/limine/releases/tag/v12.5.2)
(`limine-binary.zip`).

Files used by BuzzOS image build:

| Path | Role |
|------|------|
| `limine-bios.sys` | Stage 3 BIOS bootloader on the FAT16 boot partition |
| `limine-tool-windows-x86/limine.exe` | Host tool: `bios-install` into the disk image MBR |
| `LICENSE` | BSD-2-Clause |

`Makefile` defaults `LIMINE_DIR` to this directory. Override only when testing another package.
