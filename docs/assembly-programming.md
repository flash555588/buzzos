# BuzzOS x86_64 汇编编程指南

BuzzOS 主线是 64 位 x86_64 内核与 ELF64 用户态。仓库汇编使用 NASM
Intel 语法；旧版 i386/ELF32 示例不再属于受支持 ABI。

## 构建格式

内核汇编都位于 `src/kernel/arch/x86_64/`，以 ELF64 relocatable object
参与链接：

```powershell
nasm -f elf64 src/kernel/arch/x86_64/isr.asm -o build/obj/kernel/arch/x86_64/isr.o-asm
```

内核 C 使用 `clang --target=x86_64-none-elf`，链接器使用
`ld.lld -m elf_x86_64`. 用户程序也是 ELF64，默认加载基址为
`0x0000000100000000`。地址由 linker script 决定，因此参与链接的汇编文件
不要使用 `org`。

常见文件：

| 文件 | 作用 |
| --- | --- |
| `mb2_entry.asm` | Multiboot2 32 位入口、临时页表、进入 long mode |
| `isr.asm` | 异常、IRQ 与 `int 0x80` 入口 |
| `switch.asm` | 调度器上下文切换 |
| `setjmp.asm` | 内核 setjmp/longjmp |

Multiboot2 固件协议仍从 32 位 protected mode 进入，这是主线中唯一的
32 位启动窗口；`long_mode_start` 之后内核永久使用 64 位代码。

## x86_64 调用约定

内核 C 与汇编之间使用 System V AMD64 ABI：

- 整数或指针参数依次放在 `RDI`, `RSI`, `RDX`, `RCX`, `R8`, `R9`。
- 返回值放在 `RAX`。
- `RBX`, `RBP`, `R12`–`R15` 是 callee-saved。
- 调用 C 函数前保证栈的 16 字节对齐。
- BuzzOS 内核使用 `-mno-red-zone`；中断代码也不能依赖 red zone。
- 方向标志在进入 C 前必须清除，入口使用 `cld`。

简单的可链接函数：

```nasm
bits 64
default rel
section .text
global add_u64

; uint64_t add_u64(uint64_t a, uint64_t b)
add_u64:
    lea rax, [rdi + rsi]
    ret
```

## 启动与 long mode

`mb2_entry.asm` 完成以下顺序：

1. 保存 Multiboot magic 与信息指针。
2. 建立覆盖前 4 GiB 的临时四级页表。
3. 加载包含 64 位 code segment 的 GDT。
4. 开启 CR4.PAE、EFER.LME/NXE 与 CR0.PG。
5. far jump 到 64 位 `long_mode_start`。
6. 按 SysV ABI 用 `RDI`/`RSI` 调用 `kernel_main`。

临时页表放在 `.boot_paging`，不能放入会由 `kernel_main` 清零的 `.bss`。
内核分页初始化随后用正式四级页表替换它。

## 异常和 IRQ

`isr.asm` 使用 `SAVE_GPRS` 保存所有通用寄存器。异常桩统一在栈上补齐
vector/error-code 对，再把 vector、error code 和寄存器帧传给 C handler。
返回路径必须严格逆序恢复寄存器，并以 `iretq` 返回。

为 C handler 对齐栈时，应先保存原始 `RSP`：

```nasm
    mov r12, rsp
    and rsp, -16
    call irq_dispatch
    mov rsp, r12
```

不能把中断帧指针指向对齐后的临时栈，也不能在恢复 `RSP` 前 `iretq`。

## 系统调用 ABI

BuzzOS 为兼容现有用户 ABI，仍通过 `int 0x80` 进入内核，但寄存器和指针
都是 64 位：

| 内容 | 寄存器 |
| --- | --- |
| syscall number / return | `RAX` |
| argument 1 | `RDI` |
| argument 2 | `RSI` |
| argument 3 | `RDX` |
| argument 4 | `R10` |
| argument 5 | `R8` |

用户程序应优先调用 mini-libc wrapper。内核 syscall handler 不得直接解引用
这些寄存器给出的用户指针；必须使用 `copy_from_user`, `copy_to_user` 或
`copy_string_from_user`，让复制在分页锁下逐页验证和执行。

## 上下文切换

`switch_context(uintptr_t *old_rsp, uintptr_t new_rsp)` 从 `RDI` 取得旧栈指针
保存位置，从 `RSI` 取得新栈。它保存 flags 与所有 GPR，切换 `RSP`，再按
严格相反顺序恢复。修改保存顺序时必须同步 `task.c` 创建初始栈帧的布局。

## 用户态汇编

用户态程序必须是 ELF64、使用仓库生成的 `build/user/user.ld`，并遵守
`0x0000000100000000` 起始地址及用户空间边界。推荐通过 C 的 `crt0.c` 和
mini-libc 进入系统调用层；纯汇编程序也必须提供 `_start`、维持 SysV 栈
对齐，并最终调用 `SYS_EXIT`。

最小对象构建方式：

```powershell
nasm -f elf64 src/user/bin/example.asm -o build/user/example.o
ld.lld -m elf_x86_64 -z max-page-size=0x1000 `
  -T build/user/user.ld -nostdlib -o build/user/example.elf `
  build/user/example.o
```

把 ELF 加入 initrd 前，先运行 `python tools/check_project.py`；检查器会拒绝
ELF32、错误 machine type、越过用户范围的 segment 以及不安全 program
header。

## 调试清单

- 用 `llvm-readelf -h -l` 确认 `Class: ELF64` 与 `Machine: X86-64`。
- 早期启动故障先看 `build/serial-*.log`。
- 异常返回故障检查保存顺序、error code、栈对齐和 `iretq` frame。
- 页错误检查 CR2、PTE flags，以及复制代码是否跨页。
- 修改 ABI 后同时运行 `make host-test`, `make check-project`, `make smoke`。
