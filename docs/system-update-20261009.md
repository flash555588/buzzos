# 2026-10-09 系统平台更新

本次提交只包含内核、libc、桌面和系统测试。浏览器引擎的新增实现、
上游补丁及 HTML5 专用测试不在此公开提交内。

主要变化：物理页保留及分配失败回滚、堆和页表回收、DNS 响应匹配、
键鼠状态处理、桌面共享内存协议与输入流控、窗口和应用布局、UTF-8 字体、
libc 字符集转换，以及 Windows 用户态链接脚本生成。

桌面应用继续使用 `appui.h` 与 `guiapp.h`。新增共享协议字段放在预留头部内，
像素缓冲区偏移不变。既有浏览器源码和构建入口保留远端版本。

验证入口：`python tools/check_project.py --source-only`、
`python -m unittest discover -s tests -p "test_*.py"`、`make host-test`。
原生桌面和完整启动测试需要先生成镜像；本次整理不会沿用其他镜像的测试结果。
