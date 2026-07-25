# 文档目录

这里放的是手把手教程，不只是项目说明。

## 阅读顺序

- 第一次来：[教程与原理](tutorial.md) — 一篇从 0 到跑起来的极致详细教程。
  - 0-4. 当前能力、开发环境、仓库地图、构建镜像和运行方式
  - 5-10. shell、文件系统、进程、网络、pipe/futex 实验
  - 11-13. Limine/multiboot2 启动链、内核初始化、中断和基础驱动
  - 14-17. 内存管理、ELF 用户程序、VFS/minifs、网络栈
  - 18. framebuffer 桌面、鼠标输入、`/fs/apps` 和用户 GUI app 协议
  - 19-20. 常见问题排查和下一步扩展方向
- 想先在本地跑起来：[本地启动与引导指南](boot-guide.md) — 从仓库目录、doctor、构建、可输入 QEMU、GUI 快捷入口到常见启动问题。
- 已经进入 BuzzOS：[用户指南](user-guide.md) — shell、GUI、文本输入框、`/fs`、`/proc`、网络、IPC 和排错小抄。
- 想写或改汇编：[BuzzOS 汇编编程教程](assembly-programming.md) — 从用户态 `int 0x80` 程序到 multiboot2 入口、ISR、syscall stub 和上下文切换。
  - 用户态纯汇编 ELF 程序
  - 在 BuzzOS 内用 `nano` + `basm` 编辑、汇编、运行简单程序
  - syscall ABI：`EAX/EBX/ECX/EDX/ESI/EDI`
  - 内核汇编函数和 C 调用约定
  - multiboot2 入口、中断入口、IRQ EOI
  - `switch_context`、`setjmp/longjmp` 和汇编调试
- 想看当前项目成熟度、验证方式和下一步路线：[Project Status](project-status.md)。
- 想看用户态 GUI 和 `/fs/apps` app 示例：[User GUI Example](user-gui.md)（含 live resize / 合成妥协与 SHM stride 约定）。
- 想看阶段性项目日志和本轮成熟化记录：[Changelog](../CHANGELOG.md)。

如果你只是想快速看项目入口，再回到仓库根目录的 [README.md](../README.md)。
