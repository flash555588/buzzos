# BuzzOS 本地启动与引导指南

这份指南面向第一次在本地打开 BuzzOS 的人：先确认在仓库根目录，再检查工具，最后用可见 QEMU 窗口启动。更深入的启动链原理见 [tutorial.md](tutorial.md)。

## 1. 进入仓库根目录

PowerShell 里先切到项目目录：

```powershell
cd "C:\Users\flash\Documents\New project\buzzos"
git status --short --branch
```

如果 `make smoke` 提示 `No rule to make target 'smoke'`，通常是当前目录不在 BuzzOS 仓库根目录，或者本地不是这份更新后的分支。仓库根目录应该能看到 `Makefile`。

## 2. 检查本地工具

推荐先跑 doctor：

```powershell
make doctor QEMU="C:\Program Files\qemu\qemu-system-x86_64.exe"
```

它会检查 Python、Make、PowerShell、NASM、LLVM 工具链和 QEMU 路径。Windows 上如果 `make` 不在 PATH，可以直接用完整路径：

```powershell
& "C:\Users\flash\AppData\Local\Microsoft\WinGet\Links\make.exe" doctor QEMU="C:\Program Files\qemu\qemu-system-x86_64.exe"
```

## 3. 构建镜像

```powershell
make
```

生成的磁盘镜像是 `build/buzzos.img`。默认重建会保留旧镜像里的 `/fs` 区域；需要干净文件系统时运行：

```powershell
make image-reset-fs
```

## 4. 打开可输入的 QEMU

日常手动测试推荐：

```powershell
make run-local QEMU="C:\Program Files\qemu\qemu-system-x86_64.exe"
```

`run-local` 会打开可见 QEMU 窗口，并把串口日志写到 `build/serial-live.log`，不会让终端串口占住键盘输入。看到下面提示后，点击 QEMU 窗口再输入命令：

```text
=== BuzzOS User Shell ===
buzzos:/>
```

如果键盘没有输入效果：

- 先用鼠标点一下 QEMU 窗口内部，让焦点进入虚拟机。
- 按 `Ctrl+Alt+G` 释放或重新捕获鼠标键盘焦点。
- 不要用 `make run` 做普通手动输入测试；它用 `-serial stdio`，终端和虚拟机输入更容易混在一起。
- 如果 QEMU 卡住或镜像被占用，关掉 QEMU 后再构建；必要时在 PowerShell 里结束残留进程：

```powershell
Get-Process | Where-Object { $_.ProcessName -like "qemu*" } | Stop-Process
```

## 5. 直接打开桌面

想马上看图形界面：

```powershell
make run-gui QEMU="C:\Program Files\qemu\qemu-system-x86_64.exe"
```

这个目标会先启动 BuzzOS，再自动输入 `gui`。桌面会打开 `Applications`、`Terminal` 和 `System` 窗口；`Applications` 里可以启动 TextEdit、Paint 和 Calculator。

## 6. 引导链速览

BuzzOS 的本地启动链是：

```text
QEMU BIOS -> Limine -> multiboot2 kernel -> initrd -> /bin/sh
```

磁盘镜像布局：

```text
LBA 0           MBR / Limine BIOS stage
LBA 2048..67583 FAT16 boot partition
LBA 67584..71679 /fs minifs
```

FAT16 boot 分区里有 `kernel.elf`、`limine.conf` 和 `limine-bios.sys`。内核启动后会初始化 framebuffer、GDT/IDT、分页、物理内存、ATA、VFS、`/proc`、`/fs`、网络、调度器，然后启动用户态 shell。

## 7. 本地验证命令

常用检查：

```powershell
make check-project
make smoke QEMU="C:\Program Files\qemu\qemu-system-x86_64.exe"
make gui-smoke QEMU="C:\Program Files\qemu\qemu-system-x86_64.exe"
make verify QEMU="C:\Program Files\qemu\qemu-system-x86_64.exe"
make report
```

`make smoke` 会在 QEMU 中自动输入 shell 命令并检查串口输出。`make gui-smoke` 会自动驱动 GUI 示例并保存截图到 `build/gui-smoke`。`make report` 会生成 `build/project-report.md`，方便 review 当前镜像、GUI app、minifs、日志和验证状态。

## 8. 常见问题

`No rule to make target 'smoke'`

当前目录不对。先 `cd` 到包含 `Makefile` 的 BuzzOS 仓库根目录。

`build/buzzos.img` 被占用

QEMU 还在运行。关掉窗口或结束 `qemu-system-x86_64` 进程。

QEMU 有启动日志但输入不了

用 `make run-local` 或 `make run-gui`，点击 QEMU 窗口，把焦点交给虚拟机。避免手动测试时使用 `make run` 的串口 stdio 模式。

想确认 `/fs` 是否正常

在宿主机跑：

```powershell
make fs-check
make fs-ls
```

在 BuzzOS shell 里跑：

```text
fsinfo
fsstat
cat /proc/fs
```
