# BuzzOS 用户指南

这份指南从进入 BuzzOS shell 后开始，说明怎么使用文本 shell、GUI、用户 GUI 示例、`/fs`、`/proc` 和常见诊断命令。启动前的宿主机步骤见 [boot-guide.md](boot-guide.md)。

## 1. 进入系统

启动完成后会看到：

```text
=== BuzzOS User Shell ===
buzzos:/>
```

先试这组命令：

```text
help
about
health
interfaces
limits
fsinfo
ls /
ls /proc
ls /fs/apps
```

`about` 是项目介绍，`health` 是一屏状态，`interfaces` 是能力矩阵，`limits` 是容量边界，`fsinfo` 是 `/fs` 文件系统状态。

## 2. Shell 基本操作

常用文件命令：

```text
pwd
ls /fs
mkdir /fs/demo
write /fs/demo/hello hello
cat /fs/demo/hello
stat /fs/demo/hello
mv /fs/demo/hello /fs/demo/msg
rm /fs/demo/msg
rmdir /fs/demo
```

Shell 支持左右方向键、Home、End、Delete、Backspace、上下历史和 `Ctrl+C`。查看帮助主题：

```text
help apps
help gui
help files
help proc
help edit
help net
help pipes
```

管道和重定向示例：

```text
echo hello | cat | cat
echo saved > /fs/out
echo again >> /fs/out
cat < /fs/out
rm /fs/out
```

## 3. 运行 GUI

从 shell 输入：

```text
gui
```

GUI 会进入多窗口桌面，默认打开：

- `Applications`：显示 `/fs/apps` app 列表和 manifest 详情。
- `Terminal`：桌面内 shell 窗口。
- `System`：系统状态窗口。

窗口操作：

- 点击窗口激活并置顶。
- 拖动标题栏移动窗口。
- 拖动边缘或右下角调整窗口大小。
- 标题栏按钮支持最小化、最大化和关闭。
- 鼠标滚轮和滚动条都能滚动窗口内容。
- 在应用内容区右键可打开系统 `Copy`、`Paste`、`Cut` 菜单；剪贴板在应用间共享 UTF-8 文本。

`Applications` 里双击 app 可以打开 TextEdit、Paint、Calculator、Files 或 Browser。文本 shell 里的 `apps` 命令只查看 manifest 信息，不直接启动图形 app。

## 4. 用户 GUI 示例

BuzzOS 默认把这些用户态 GUI 程序种子到 `/fs/apps`：

- `textedit`：文本编辑器，保存到 `/fs/textedit.txt`，编辑区随窗口大小变化。
- `paint`：绘图程序，画布和工具栏随窗口大小变化。
- `calculator`：表达式计算器。
- `filemanager`：文件管理器，可把普通文件交给 TextEdit 打开。
- `browser`：简单 HTTP 浏览器，支持历史、重定向、正文滚动和 UTF-8 页面文字。

Files 双击 ELF 时会检查 GUI manifest：真正的 GUI app 作为窗口启动，普通
命令行 ELF 自动切到 Terminal 执行，不会再被误当成 GUI 程序而卡住桌面。

GUI 文字统一按 UTF-8 解码，支持常用中文、拉丁扩展、希腊文、俄文、
日文假名和常用标点。用 Files 打开 `/fs/utf8.txt` 可以查看内置多语言
样例；TextEdit 的左右移动和退格会按完整 UTF-8 字符处理。

桌面内按 `Ctrl+Space` 切换右上角的 `英`/`中` 状态。中文状态下输入全拼，
按 Space 或 Enter 选择第一个候选，或按 `1` 到 `9` 选择对应候选；Backspace
修改拼音，Escape 取消组合。候选栏和组合状态由桌面统一管理，TextEdit、
Browser、Files 对话框、Calculator 和 Terminal 都通过同一个 UTF-8 文本事件接收结果。
Terminal 的命令行编辑器不会再丢弃 UTF-8 字节，左右移动、Backspace 和 Delete
都按完整字符处理。终端中按住鼠标左键可跨行选择并显示高亮，右键 Copy
复制选区；没有选区时 Copy/Paste/Cut 操作当前输入行。

通过 app 模型查看：

```text
apps
apps info textedit
apps info paint
apps info calculator
```

TextEdit 文本框操作：

- 鼠标点击输入框聚焦。
- Backspace/Delete 删除字符。
- Left/Right/Up/Down/Home/End 移动光标。
- Enter 插入换行。
- 拖动水平/垂直滚动条或使用鼠标滚轮滚动。
- 在正文中按住鼠标左键拖动可选择文字，右键后可复制、粘贴或剪切。

## 5. `/proc` 状态文件

`/proc` 是只读运行状态，不占用 `/fs` 空间：

```text
cat /proc/about
cat /proc/health
cat /proc/interfaces
cat /proc/limits
cat /proc/fs
cat /proc/tasks
cat /proc/threads
cat /proc/meminfo
cat /proc/fds
cat /proc/net
cat /proc/sync
cat /proc/mounts
```

对应快捷命令：

```text
about
health
interfaces
limits
fsinfo
fdstat
netstat
syncstat
threads
```

`/proc/fs` 输出 `/fs` mount、minifs 状态、inode/block 用量、数据区 LBA、最大文件大小，以及宿主机检查/修复入口：

```text
cat /proc/fs
fsinfo
fsstat
```

`fsinfo` 读取 `/proc/fs`，适合看统一状态；`fsstat` 直接走文件系统 syscall，适合确认内核 minifs 计数。

## 6. 网络与 IPC 示例

QEMU user network 启动后会自动 DHCP。常用命令：

```text
netstat
ping 10.0.2.2
wget 10.0.2.2 8000
dhcp
```

IPC 和同步测试：

```text
pipetest
pipeedgetest
pipeblocktest
futextest
futextimeouttest
futexcanceltest
futexblocktest
```

`futexblocktest` 会展示 `/proc/threads` 和 `/proc/sync` 中的阻塞等待状态。

## 7. 在系统内写小程序

BuzzOS 带有 `nano` 和 `basm`：

```text
nano /fs/demo.asm
basm /fs/demo.asm /fs/demo
exec /fs/demo
```

在 `nano` 里：

- `Ctrl+T` 插入最小汇编模板。
- `Ctrl+S` 保存。
- `Ctrl+C` 退出。

`basm` 是教学用小 assembler，不是完整 NASM。它输出 BuzzOS loader 可执行的 ELF32 文件。

## 8. 排错小抄

看项目和系统状态：

```text
about
health
interfaces
limits
fsinfo
```

看进程、线程、fd、网络和同步：

```text
ps
threads
fdstat
netstat
syncstat
```

看文件系统：

```text
ls /fs
fsinfo
fsstat
cat /proc/fs
```

如果 GUI 里输入框不响应，先点击输入框，或按 `Tab` 切换焦点。若整个 QEMU 不吃键盘输入，回到宿主机按 [boot-guide.md](boot-guide.md) 的 QEMU 焦点步骤处理。
