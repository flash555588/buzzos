# BuzzOS x86_64 教程

BuzzOS 是一个教学型 x86_64 POSIX-like OS。当前受支持的首个里程碑是
BIOS/Limine、单核、long mode、ELF64 内核和 ELF64 用户态。本文只描述当前
主线；旧版 i386/ELF32 构建步骤已经移除。

## 1. 准备环境

完整构建与 QEMU gate 的标准宿主机是 Windows；Linux 支持 source check 和
host tests。版本和覆盖变量见 [dependencies.md](dependencies.md)。

```powershell
make doctor QEMU="C:\Program Files\qemu\qemu-system-x86_64.exe"
powershell -NoProfile -ExecutionPolicy Bypass -File tools/fetch-netsurf.ps1
make -j2
```

重要覆盖变量：`PYTHON`, `CC`, `LD`, `OBJCOPY`, `NASM`, `HOST_CC`,
`HOST_CC_ARGS`, `QEMU`, `QEMU_MEMORY`。NetSurf 和依赖只通过 HTTPS 获取，
revision 固定在 fetch script 中。

## 2. 仓库结构

```text
src/kernel/arch/x86_64/  long-mode entry, GDT/IDT, paging, IRQ
src/kernel/core/         kernel entry, ELF64 loader, exec
src/kernel/mm/           physical page allocator
src/kernel/sched/        processes, threads, scheduler
src/kernel/syscall/      stable syscall table and handlers
src/kernel/fs/           VFS, procfs, ramfs, devfs, minifs
src/kernel/net/          NIC, validated packet views, protocols
src/user/libc/           freestanding user ABI and UI helpers
src/user/bin/            shell, desktop and applications
scripts/                 QEMU runtime gates
tests/                   deterministic host tests
tools/                   image, filesystem and consistency tools
```

## 3. x86_64 启动链

```text
BIOS -> Limine -> Multiboot2 32-bit entry -> temporary PML4
     -> enable long mode -> x86_64 kernel_main -> /bin/sh -> gui
```

Multiboot2 入口位于 `src/kernel/arch/x86_64/mb2_entry.asm`。它在短暂的 32 位
协议环境中建立 4 GiB identity map，开启 PAE/LME/NXE/paging，然后 far jump
进入 64 位代码。C 内核使用 System V AMD64 ABI 和四级页表。

镜像布局：

```text
LBA 0             MBR / Limine BIOS stage
LBA 2048..67583   FAT16 boot partition
LBA 67584..       persistent minifs partition
```

`tools/mkbootimg.py` 写入 kernel ELF、Limine 文件和持久化分区。普通重建保留
已有 `/fs`；只有 `make image-reset-fs` 明确重建它。

## 4. ELF64 用户态与 syscall

用户 ELF 链接起点为 `0x0000000100000000`，与内核低地址映射隔离。loader
验证 ELF class、machine、program header、segment bounds、文件长度与用户
地址范围后才映射 segment。

现有 syscall number 保持兼容。用户 libc 用 64 位 `int 0x80` ABI：number
在 `RAX`，前五个参数在 `RDI`, `RSI`, `RDX`, `R10`, `R8`。所有来自用户态
的 buffer 和字符串必须经过逐页 `copy_from_user`, `copy_to_user`,
`copy_string_from_user`; handler 不直接解引用经过一次检查的用户地址。

## 5. 内存、调度与锁

E820 初始化 bitmap PMM，paging 为每个进程维护独立地址空间。内核仍是单核
但可抢占。当前页级 user copy 在 paging lock 下验证并复制，避免另一个线程
在 validation 与 dereference 之间改变 mapping。

在进入 SMP 之前，需要把共享全局状态完整迁移到明确的 IRQ spinlock、
non-sleeping spinlock 和 scheduler mutex/wait queue，并建立锁顺序测试。

可配置 RAM smoke：

```powershell
make smoke QEMU_MEMORY=64
make smoke QEMU_MEMORY=256
make smoke QEMU_MEMORY=1024
make smoke QEMU_MEMORY=3072
```

## 6. 网络边界

NIC 收到的原始 frame 先进入纯 parser。Ethernet/IPv4/ICMP/UDP/TCP view 在
返回前检查 received length、IPv4 version/IHL、declared total length、
fragment policy、transport length/data offset 和 checksum。协议代码只能读取
validated view 中的范围。

`/proc/net` 包含 `invalid_frames`, `checksum_failures`, `fragment_drops`。
host test 覆盖截断 IPv4、超长 UDP length、短 ICMP、错误 TCP data offset、
fragment 和 checksum。后续工作仍包括单一 NIC dispatcher 与完整 per-protocol
queue，避免不同接收者竞争同一个 raw ring。

## 7. minifs 边界

minifs 只在 superblock sector 完全空白时自动格式化。未知 magic、损坏的布局
或非空垃圾会 fail closed，不写磁盘。`/proc/fs` 暴露 mounted、dirty、corrupt、
recovery_required、journal 和 last_error。

minifs v1 仍可读/迁移。当前尚未声称 crash consistency：journal 显示
`disabled`，clean/dirty marker 和 reset-at-every-write journal gate 是后续阶段。

宿主检查：

```powershell
make fs-check
make fs-check-negative
make fs-check-repair
```

## 8. 诊断接口

`/proc/about` 的固定 ABI 身份是：

```text
kind native-x86_64-posix-os
arch x86_64
mode long64
```

常用文件包括 `/proc/health`, `/proc/interfaces`, `/proc/limits`, `/proc/fs`,
`/proc/net`, `/proc/tasks`, `/proc/threads`, `/proc/fds`。shell 和 desktop terminal
读取同一 procfs 数据，避免重复状态实现。

## 9. 验证

快速检查：

```powershell
make host-test
python tools/check_project.py --source-only
python tools/check_project.py
make smoke QEMU="C:\Program Files\qemu\qemu-system-x86_64.exe"
```

完整 release gate：

```powershell
make verify QEMU="C:\Program Files\qemu\qemu-system-x86_64.exe"
make release VERSION=0.1.0 QEMU="C:\Program Files\qemu\qemu-system-x86_64.exe"
```

PR CI 在 Windows 构建 image 并执行 serial smoke，同时在 Windows/Linux 执行
behavioral host tests。nightly 增加 RAM matrix、filesystem corruption/repair、
network stress 与 GUI smoke。release 目录包含 versioned image、SHA-256、工具与
依赖 manifest、reproduction instructions 和现有 serial logs。

## 10. 继续加深系统

x86_64 baseline 全绿之后，推荐顺序是：

1. 完成 NIC dispatcher、per-protocol/socket queues 和 close cancellation。
2. clean/dirty superblock、ordered metadata update、小型 metadata journal。
3. socket 纳入 fd table，结构化 errno、nonblocking、poll/select。
4. COW fork、execve、signals、process groups、terminal job control、permissions。
5. behavioral coverage 后拆分网络、shell、desktop monolith。
6. 最后才进入 SMP/per-CPU scheduler、UEFI、virtio-blk/net 与更多硬件。

每一步都应新增行为测试，不使用 source-string match 代替运行时不变量。
