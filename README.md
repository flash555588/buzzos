# BuzzOS

BuzzOS 是一个面向学习和实验的 i386 POSIX-like 操作系统。它已经演进成一个能启动用户态 shell、运行多任务、挂载持久化文件系统、联网，并通过 Limine framebuffer 显示桌面的完整小型系统。

English: [README.en.md](README.en.md)

相关文档：[CHANGELOG.md](CHANGELOG.md)、[本地启动指南](docs/boot-guide.md)、[用户指南](docs/user-guide.md)、[项目状态](docs/project-status.md)、[用户 GUI app 指南](docs/user-gui.md)。运行 `make report` 可以生成本地验证报告 `build/project-report.md`。

<table>
  <tr>
    <td><img src="/pic/demo1.png" alt="BuzzOS demo 1" width="480"></td>
    <td><img src="/pic/demo2.png" alt="BuzzOS demo 2" width="480"></td>
  </tr>
  <tr>
    <td><img src="/pic/demo3.png" alt="BuzzOS demo 3" width="480"></td>
    <td><img src="/pic/demo4.png" alt="BuzzOS demo 4" width="480"></td>
  </tr>
</table>

## 当前状态

- 启动链路：Limine BIOS + multiboot2，默认请求 `1280x800x32` framebuffer。
- 内核：GDT、IDT、异常处理、PIC、PIT、串口、PS/2 键鼠、分页、E820/PMM、ELF32 loader。
- 用户态：`/bin/sh` shell、`nano`、`basm`、`cat`、`echo`、`gui`。
- 桌面：用户态多窗口桌面，支持窗口激活置顶、拖动、缩放、最小化、最大化、关闭和滚动条。
- 图形加速：virgl 下按窗口直接 GPU 合成，普通应用 SHM 零 CPU 拷贝导入；GPU Canvas 应用可提交安全的矩形/圆角/UTF-8 文本显示列表，桌面空闲时采用事件驱动休眠。
- GUI app：System Monitor、Terminal、TextEdit、Paint、Calculator、Files、Browser 和媒体/游戏示例作为独立用户 ELF 程序，通过 `/fs/apps/*.app` manifest 注册。
- 文件系统：VFS + initrd/ramfs + devfs + 持久化 minifs，`/fs` 默认随镜像重建保留。
- 网络：QEMU NE2000、DHCP、DNS、ICMP、UDP、TCP client 和用户态 socket API。
- IPC/同步：pipe、阻塞读写唤醒、futex wait/wake。
- 诊断接口：`/proc/about`、`/proc/health`、`/proc/interfaces`、`/proc/limits`、`/proc/fs`，并暴露到文本 shell、桌面终端和 `make report`。

## 快速运行

需要工具：

| 工具 | 用途 |
| --- | --- |
| `nasm` | 汇编内核入口和中断桩 |
| `clang` | 编译 freestanding C 内核和用户程序 |
| `ld.lld` | 链接内核和 ELF 用户程序 |
| `llvm-objcopy` | 生成辅助二进制产物 |
| `python` | 生成 initrd、app registry、磁盘镜像 |
| `powershell` | Windows 下运行脚本 |
| `qemu-system-i386` | 运行 BuzzOS |
| Limine（已 vendoring） | 仓库内 `third_party/limine/`，安装 BIOS 启动阶段 |

默认使用仓库内的 Limine（v12.5.2）：

```text
third_party/limine/limine-bios.sys
third_party/limine/limine-tool-windows-x86/limine.exe
```

一般无需额外下载。若要用自备包，构建时传 `LIMINE_DIR=...`。

查看推荐的本地工作流：

```sh
make help
```

检查本机环境：

```sh
make doctor QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
```

该目标调用 `tools/doctor.py`，也可以给脚本传 `--soft` 只报告问题而不中止。

构建并运行：

```sh
make
make run
```

使用 Intel HD Audio 控制器和 HDA duplex codec 启动：

```sh
make run-hda
```

在可见 QEMU 窗口运行，并把串口日志写入 `build/serial-live.log`：

```sh
make run-local QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
```

直接启动后进入桌面：

```sh
make run-gui QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
```

重建镜像并清空 `/fs`：

```sh
make image-reset-fs
```

如果 `build/buzzos.img` 或 `build/user/*.o` 被占用，通常是 QEMU 还在运行。关掉 QEMU 后再构建。

## 桌面和 App

文本 shell 中输入：

```text
gui
```

桌面默认打开：

- `Applications`：应用列表和 manifest 详情。
- `Terminal`：桌面内 shell 窗口。
- `System`：系统状态面板。

窗口支持：

- 点击激活并置顶。
- 拖动标题栏移动。
- 拖动边缘或角落调整大小。
- 最小化、最大化、关闭。
- 鼠标滚轮和滚动条。
- Dock 使用 app 声明的窗口标题；应用较多时可通过 More 展开完整任务列表，悬停可查看完整标题。
- 桌面最多同时承载 10 个外部 GUI app 窗口，另有 Apps、Terminal 和 System 三个系统窗口。

默认用户 GUI app：

| App | 说明 |
| --- | --- |
| System Monitor | 实时、可排序的进程列表，显示 PID、状态、CPU 和驻留内存；提供资源历史图、暂停/刷新和带确认的结束进程操作 |
| TextEdit | 文本编辑器，编辑区随窗口大小变化，支持回车、光标移动、水平/垂直滚动条，保存到 `/fs/textedit.txt` |
| Paint | 位图绘图工具，画布和工具栏随窗口大小变化，支持画笔、橡皮、直线、矩形、填充和连续笔画 |
| Calculator | 表达式计算器，支持括号、小数和常见四则表达式 |
| Files | 文件管理器，支持目录导航、新建、重命名、删除，以及用 TextEdit 打开普通文件 |
| Browser | 简单 HTTP 浏览器，支持历史、重定向、滚动和 UTF-8 网页正文 |

GUI 使用 UTF-8 文本管线，覆盖常用中文、拉丁扩展、希腊文、俄文、日文假名和标点。`/fs/utf8.txt` 是内置多语言样例；TextEdit 会按完整 UTF-8 字符移动和退格。桌面内按 `Ctrl+Space` 切换系统拼音输入法，Space/Enter 选首项，数字 `1`–`9` 选候选。

Text shell 里可以查看 app 信息：

```text
apps
apps info textedit
apps info paint
apps info calculator
apps info filemanager
```

GUI app 由桌面启动，不再从文本 shell 里直接运行图形 app。

## 写自己的 GUI App

新增 app 的推荐路径：

```sh
make new-app APP=myapp
```

然后：

1. 在 `src/user/bin/myapp.c` 实现用户态 app。
2. 在 `src/user/bin/myapp.app` 写 manifest。
3. 把 `myapp` 加进 `Makefile` 的 `GUI_APP_NAMES`。
4. 运行：

```sh
make app-registry
make app-check
make image-reset-fs run
```

GUI app 协议在 [src/user/libc/guiapp.h](src/user/libc/guiapp.h)，基础控件绘制在 [src/user/libc/appui.h](src/user/libc/appui.h)。桌面通过 pipe 发送初始化、resize、mouse、key、close 事件，app 返回完整帧或 dirty rect。

## Shell 常用命令

启动后进入：

```text
=== BuzzOS User Shell ===
buzzos:/>
```

常用命令：

```text
help
about
health
interfaces
limits
fsinfo
fsstat
fdstat
ls [path]
cd [path]
pwd
stat <path>
cat <file>
mkdir <dir>
rmdir <dir>
touch <file>
write <file> <text>
rm <file>
mv <old> <new>
nano <file>
basm <input.asm> [output]
gui
apps [list|info <name>]
exec <program> [args...] [&|bg]
wait [pid]
kill <pid>
ps [-a]
echo <text>
sleep <seconds>
reboot
```

网络和 IPC 测试：

```text
ping <host-or-ip>
wget <host> [port]
tcptwotest <host> <port-a> <port-b>
dhcp
netstat
pipetest
pipeedgetest
pipeblocktest
futextest
futextimeouttest
futexcanceltest
futexblocktest
elfbadtest
```

在 BuzzOS 内写汇编并运行：

```text
nano /fs/demo.asm
basm /fs/demo.asm /fs/demo
exec /fs/demo
```

`nano` 中 `Ctrl+T` 可插入最小汇编模板，`Ctrl+S` 保存，`Ctrl+C` 退出。

## 镜像布局

`build/buzzos.img` 是 raw 磁盘镜像：

| 区域 | 用途 |
| --- | --- |
| LBA 0 | MBR，Limine BIOS stage 安装在这里 |
| LBA 2048..67583 | FAT16 boot 分区，包含 `kernel.elf`、`limine.conf`、`limine-bios.sys` |
| LBA 67584..71679 | raw `/fs` minifs 分区，默认 4096 sectors / 2 MiB |

`tools/mkbootimg.py` 默认保留旧镜像的 `/fs` 区域。需要清空时运行 `make image-reset-fs`。

检查和修复 `/fs`：

```sh
make fs-check
make fs-ls
make fs-repair
```

`make fs-repair` 默认写出 `build/buzzos-repaired.img`，不会覆盖当前镜像。

## 验证

快速一致性检查：

```sh
make check-project
```

串口 smoke：

```sh
make smoke QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
```

GUI smoke：

```sh
make gui-smoke QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
```

完整验证：

```sh
make verify QEMU="C:\Program Files\qemu\qemu-system-i386.exe"
```

## 设计边界

BuzzOS 仍然是教学和实验系统，不是完整 Unix：

- 没有 `fork/execve`、权限模型、信号、动态链接和成熟设备模型。
- 网络栈是轻量 client 实现；TCP 已有累计 ACK、接收窗口和有限重传，但还没有拥塞控制、选择确认和成熟的长期连接管理。
- minifs 是固定区域、无日志的小文件系统。
- 桌面是用户态窗口管理器，不是独立 GUI server；app 协议已经拆出来，后续可以继续演进。
- syscall 用户指针会逐页校验 present/user/read-write 权限，但还没有完整的 `mmap`/按需分页模型。

## 代码入口

- Kernel entry: [src/kernel/core/kernel.c](src/kernel/core/kernel.c)
- Multiboot2 entry: [src/kernel/arch/i386/mb2_entry.asm](src/kernel/arch/i386/mb2_entry.asm)
- Framebuffer driver: [src/kernel/drv/fb.c](src/kernel/drv/fb.c)
- Scheduler/processes: [src/kernel/sched/task.c](src/kernel/sched/task.c)
- Syscalls: [src/kernel/syscall/syscall.c](src/kernel/syscall/syscall.c)
- Graphics syscall: [src/kernel/syscall/sys_gfx.c](src/kernel/syscall/sys_gfx.c)
- VFS core: [src/kernel/fs/vfs.c](src/kernel/fs/vfs.c)
- Mini FS: [src/kernel/fs/minifs/minifs.c](src/kernel/fs/minifs/minifs.c)
- Network stack: [src/kernel/net/net.c](src/kernel/net/net.c)
- User shell: [src/user/bin/shell.c](src/user/bin/shell.c)
- Desktop: [src/user/bin/gui.c](src/user/bin/gui.c)
- TextEdit: [src/user/bin/textedit.c](src/user/bin/textedit.c)
- Paint: [src/user/bin/paint.c](src/user/bin/paint.c)
- Calculator: [src/user/bin/calculator.c](src/user/bin/calculator.c)
- GUI app protocol: [src/user/libc/guiapp.h](src/user/libc/guiapp.h)
- User libc: [src/user/libc/libc.c](src/user/libc/libc.c)
