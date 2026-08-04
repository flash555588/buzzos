# x86_64 Build and Dependency Manifest

Windows is the canonical host for complete image builds and QEMU release gates.
Linux remains supported for `make host-test` and source consistency checks.

The validated development environment for the x86_64 milestone is:

| Component | Validated version | Override |
| --- | --- | --- |
| Python | 3.13.14 | `PYTHON=...` |
| LLVM Clang / LLD | 22.1.8 | `CC=... LD=...` |
| LLVM objcopy | LLVM 22 | `OBJCOPY=...` |
| NASM | 3.02 | `NASM=...` |
| Host C compiler | Clang 22 or Zig 0.16 `cc` | `HOST_CC=... HOST_CC_ARGS=...` |
| QEMU | 11.0 x86_64 | `QEMU=...` |
| GNU Make | 4.x | command-line host tool |
| PowerShell | 5.1+ | Windows scripts |
| Limine | 12.5.2, vendored | `LIMINE_DIR=...` |

All NetSurf inputs are fetched over HTTPS at immutable Git revisions. The
authoritative pins are in `tools/fetch-netsurf.ps1`; the top-level NetSurf pin
is `a471a0d44274ec57fee5e5f30ae59fbd2ad02656`. The script also pins
buildsystem, libparserutils, libwapcaplet, libhubbub, libdom, libcss,
libnsutils, libnsfb, and zlib independently.

Canonical clean-clone workflow:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/fetch-netsurf.ps1
make -j2
make verify QEMU="C:\Program Files\qemu\qemu-system-x86_64.exe"
make release VERSION=0.1.0 QEMU="C:\Program Files\qemu\qemu-system-x86_64.exe"
```

`make release` creates a versioned image, SHA-256 checksum, dependency JSON,
reproduction instructions, and copies available serial logs into
`build/release/<version>/`.
