# 静态 shader / 管线预编译（Demon's Souls，PPSA01341）

目标：不靠实际游玩录制，直接从游戏文件收集所有会用到的 shader 和管线，提前编译进驱动缓存，
游戏里第一次遇到新 shader / 管线时不再卡 1–3 秒。

## 流程

1. `precompile.py seeds OUT`：静态收集，写种子文件（`KytyShaderSeeds1`，与 warmup 缓存同格式）。
   - shader：所有 CSDR 包（`_trinity` 为 PS5 Pro 版本，跳过）+ eboot.bin 数据段里内嵌的 AGC 头与代码。
   - 编译 key：由 AGC 头的寄存器表推导（`keys.py`，移植自 `shader.cpp`/`pm4Handlers.cpp`/`agc.cpp`；
     CS 用 dispatch_modifier，PS 插值器由 VS/PS 语义配对算出）。
   - VS/PS 配对：材质 `.cmat` 的 technique 列表（pass id + 包内条目）；引擎自身的全屏/UI/粒子 pass 按
     `coredata/enginesupport/shaders/<效果>/` 目录配对。
   - 管线状态：每个材质 pass 的渲染目标/混合/深度模板状态表（`pass-states.json`，由
     `learn-states` 从录制缓存学得的“引擎渲染 pass 配置”），每种状态再生成 cull 开/关两种。
2. `precompile-windows.ps1`（仓库根目录）：独立程序 `kyty_shader_precompile`（不开游戏、不开窗口；
   `build-windows.cmd kyty_shader_precompile` 编译）把每个种子翻译成 SPIR-V（便携特化，见下），创建
   全部管线，写入**静态管线缓存** `_PipelineCache/static/PPSA01341.bin`。种子文件不存在时先跑第 1 步。
   - 静态缓存只按 GPU/驱动签名，跨模拟器重编保留；游戏建管线先在其中免编译查找
     （`VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT`），命中即返回。
   - NVIDIA 驱动在一个进程内编译大型 CS 近乎串行（22 线程只忙 1.3 核，大的每条约 7 秒），所以按
     `--shard i/n` 分成多个低优先级进程并行（`-Jobs`，默认每个允许的 CPU 一个），最后合并。
   - 重跑只编译缺的（静态缓存里有的直接命中）。各进程每 10 分钟存一次检查点，中断后重跑先合并这些检查点。
   - `-Coverage`：不建管线，只写出编译了什么（`<seeds>.compiled.shaders` 及 `.spirv.txt`/`.rejected.txt`）。
   - 全量跑完最后再写 `_PipelineCache/static/PPSA01341.shaders`（编译用的输入，warmup 格式，136 MB），供游戏的
     **后台预翻译**（`KYTY_SHADER_PREFETCH`，默认开）：游戏启动后低优先级线程把这些程序全部翻译好（约 60–100 s，
     SPIR-V 放系统临时文件），第一次遇到时直接取用，不再现场翻译。`-InputsOnly` 只写这个文件（约 30 s）。
     它存的是编译输入（翻译用当时的模拟器做），种子或特化推断（`SeedSpecializations`）变了才需要重写。
   - 对比用：`KYTY_STATIC_PIPELINE_CACHE=0` 让游戏不加载静态缓存，`KYTY_SHADER_PREFETCH=0` 不做后台预翻译。
3. `precompile.py coverage COMPILED --recorded-compiled R`：与录制缓存对比（SPIR-V 级＝驱动缓存能否命中）。
   `recorded-seeds` 把录制缓存转成种子，同样用 `-Coverage` 编译得到 R。
4. `stalls.py LOG`：运行日志里的编译卡顿统计（`KYTY_SLOW_LOG_MS=30`）。

## 便携特化（recompiler，`KYTY_PORTABLE_SHADERS`，默认开）

shader 二进制里没有、只在运行时描述符里的信息原先会被烘焙进 SPIR-V，静态无法预测：

- buffer stride（≤4095、非 swizzle）：改为读 shader data 里每个 buffer 的描述字（`IR::BufferWord`）。
- 只读的格式化 buffer（顶点属性等）的格式/dst_sel：两级。首次遇到时用“运行时解码格式”的便携模块 P
  （静态预编译的正是它，无驱动编译卡顿），warm 缓存记下专用特化 S，下次启动预热时编译 S（满速）。
- null 纹理：按 shader 声明的维度（原先一律 2D，导致同一程序多出一个排列）。
- 间接纹理表：候选数补齐到 2 的幂，键映射偏移经 flattened SRT 头部读取，二分搜索改为运行时循环。
  表的类型取自表里的纹理：粒子 CS 的表指令声明 3D，游戏放的却都是 2D 纹理，所以种子对每个变体再各出
  一份 2D 表的版本。

仍会烘焙的：整数纹理的数值类型（`PredictImageNumericClasses` 只能从用法猜到一部分）、存储图像 swizzle、
cube、被写入的格式化 buffer 的格式（按最常见的 32UInt 猜）、stride ≥4096 或 swizzle 的 buffer。

## 游戏内实测（09-30）

方法：`tools\local\windows\bench-run.ps1 -NoPrecompile -Set KYTY_SHADER_WARMUP=0,KYTY_SLOW_LOG_MS=30`
（冷启动：不预热、驱动缓存为空），然后
- `stalls.py LOG`：编译卡顿次数与总时长；
- `precompile.py runtime-coverage LOG COMPILED`：运行时编出的模块（日志里的 `MODULE` 行，SPIR-V 哈希）有多少在
  预编译集合里（COMPILED 为 `precompile-windows.ps1 -Coverage` 的输出）。
- `KYTY_SHADER_WARMUP=record` + `KYTY_SHADER_WARMUP_FILE=<新文件>` 可把一次运行编过的输入单独录下来，
  再用 `recorded-seeds` + `-Coverage` + `coverage --recorded` 看缺的模块差在哪个特化字段。

固定路线结果：运行时编出的模块 CS 82%、PS 91%、VS 100% 在预编译集合里（运行时用到的程序全部在种子里，
缺的都是特化推断不同）。缺的 CS 在真正冷启动时约 36 条管线、共 22 s；NVIDIA 驱动的磁盘缓存与游戏自己的
`_PipelineCache/local/<exe SHA256>` 都会让重复测试偏"暖"（同一批管线 30 ms 或 100+ ms），比较时看模块覆盖率更可靠。

没命中的管线不再卡住：先以未优化方式编译（NVIDIA 上毫秒级）立即使用，后台编优化版替换
（`KYTY_PIPELINE_FAST_BUILD=0` 关闭）。冷启动路线管线卡顿从 22 s 降到 0。

缺的特化（CS 为主）：整数纹理被判成浮点（已加"合并前的直接用途全是整数运算即判 uint"）、存储图像的单通道
swizzle（X001）、声明 2D 数组但绑定 cube 或普通 2D 纹理、被写的格式化 buffer 实际不是 32UInt。

## 规模

- 游戏自带 20.6K 个程序（CS 10.7K，其中粒子 9.5K；PS5 机器码合计 71 MB）、约 4.6 万条管线；
  实际玩一遍只用约 3K。粒子 CS 是真正不同的程序（放宽到 10% 指令不同也只能并成 7.6K 组），省不掉。
- NVIDIA 缓存里每条管线约为 PS5 原码的 26 倍（翻译开销、16 字节定长指令、图形管线各带一份 VS+PS），
  全量估计 3–5 GB。

## 已知缺口

- 整数纹理的数值类型：使用情况预测只认出约 23%（CS 约 10% 的程序因此不命中）。
- pass 10（1799 对 VS/PS）从未出现在录制里，渲染目标格式/混合未知，未生成管线。
- 3 个以上间接纹理源的程序只编单纹理形式。
- 8 个种子被 recompiler 拒绝（游戏不会用到的代码路径），见 `*.rejected.txt`。

## 便携格式解码的校验

`shader_recompiler_compute_tests --portable-formats-only` 对每种格式、wave32/64 在 GPU 上比较便携（运行时）解码与
按描述符专用的解码；归一化格式有已知的 FDiv 舍入差异（最多 2 ULP），会如实报告（`--portable-specialization-parity-only`
逐位严格比较，目前因此失败）。`--portable-formats-emission-only` 只编译不执行。

试过把少见格式的解码放进一个共享函数（`DontInline`，09-29 夜）：大 shader 的编译时间有增有减、总量基本不变，而且
`b4a64911fc8e88db` 这类模块会让 NVIDIA 编译器（nvgpucomp64.dll）崩溃，游戏冷遇到即闪退，已删除。
本机 Windows NVIDIA 的驱动级 shader 缓存不受 `__GL_SHADER_DISK_CACHE=0` 控制，重复编译同一 SPIR-V 不能当冷编译对照。
