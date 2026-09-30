# 本地 PGO

当前正式构建：`_Build/agent30fps-20260917/build/`，Clang 18 / Release / ThinLTO。
其 profile 输入是 `_Build/pgo/walk8-20260915-v5/kyty.profdata`，不是可随测试日志删除的临时文件。
实际编译/链接参数以该构建的 Ninja 命令和 CMakeCache 为准。

重新训练时使用独立目录，保持训练和使用时的源码一致：

1. 用 `-fprofile-instr-generate` 构建训练版本；若正常退出走 `std::quick_exit`，训练版本额外链接 `pgo-quick-exit.cpp`，确保 profile 写出。
2. 设置 `LLVM_PROFILE_FILE` 到新的输出目录，正常运行代表性场景并退出。训练版本 FPS 不是收益结果。
3. 用 `llvm-profdata-18 merge` 合并确认属于本次成功运行的非空 profraw。
4. 在独立构建中使用 `-fprofile-instr-use=/绝对路径/kyty.profdata`，记录源码和二进制指纹，按 [同进程测量方法](../../docs/BENCHMARKING.md) 验证。

不要在 FPS 测量期间编译或训练，不要覆盖正在运行或正式交付的二进制。mimalloc 是独立宿主分配器选择，不能把更换分配器与 PGO 收益混为一谈。
