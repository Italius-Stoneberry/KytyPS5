# 当前可玩版本

更新：2026-09-20。正式运行版本没有因 PBR 实验改变，重场景约 22～23 FPS。

```bash
./run.sh                 # 2560×1440，手动选择 Continue / New Game
./run.sh --dry-run       # 验证路径、只打印启动命令
python3 tools/local/play-demons-souls.py --2k  # 带运行日志的同一正式配置
```

唯一正式配置：`_Build/agent30fps-20260917/launch-wins7.json`。
`_Build/profiles/manual-current-launch.json` 仅作为指向该配置的兼容符号链接，不再维护另一套旧配置。

| 项目 | 当前值 |
|---|---|
| 正式二进制 | `_Build/agent30fps-20260917/build/kyty_emulator` |
| SHA256 | `78a0d08fc4f8d3698a8173232e6cbaa527344661f85a021a447a4033e2ac6daa` |
| 基线源码 | `4e4e5074205d5d20c1d3fe4ae1bc2010acd940ba` |
| 当前工作树 | PBR 实验源码已在 2026-09-25 历史整理中删除（保留在分支 `experiment/render-preparation-20260912`，提交 `4134ca6`）；仍含其他默认关闭的实验开关，不是干净基线 |
| AOT | `_Build/srt-aot/walk-libraries/14327ed133b2018689d9d6554056dcf595bfce94a34daa65a5a57037ab4035c2/srt-aot.so` |
| 分配器 | `_Build/profiles/libraries/b0f267c17975ee39391331d0f02dd3682233a67e8464095db1407cd4e37f3853/libmimalloc.so` |
| 绑核 | renderer CPU 0；其他游戏线程 CPU 1–3、6–23 |

必须保留：

- 上述正式构建目录、配置、AOT 和分配器库。
- `_Build/pgo/walk8-20260915-v5/kyty.profdata`：正式构建的 PGO 输入。
- `_Build/walk-fps-20260914/save-baseline/`：固定测试存档。
- `_SaveData/`：用户真实存档；不作为实验垃圾清理。
- `_PipelineCache/` 的通用预热数据，以及 `local/78a0d08f…/` 正式二进制的驱动缓存。
- `_Build/deps/`、`_Build/debug-tools/`：构建依赖和菜单识别工具。

测试时显示器保持活动、60 Hz；诊断计数默认关闭。轻量运行日志由启动器写入 `_Build/run-logs/`，旧日志可清理。

历史实验和当前 PBR 结果统一见 [实验简记](EXPERIMENTS.md)，复测流程见 [测量方法](BENCHMARKING.md)。

## Windows

```powershell
.\build-windows.cmd                              # clang-cl 19 + Ninja：-O3 -march=native + ThinLTO + PGO，输出 _Build\windows\kyty_emulator.exe
.\run-windows.ps1 -Fullscreen -AspectFit         # 日常：release-stage1 全部开关，全屏，16:9 画面 1:1 居中（5120×2160 屏两侧黑边）
.\run-windows.ps1 -Width 1920 -Height 1080       # 窗口模式（画面拉伸铺满窗口）
.\run-windows.ps1 -Precompile                    # 预编译全部已记录的 shader/管线后退出（换 exe 后首次约 7 s，冷驱动缓存约 2 min）
.\run-windows.ps1 -DryRun                        # 只打印环境变量和命令
```

其他参数：`-Baseline`（不开性能开关）、`-NoAot`、`-NoRedZone`、`-Set KEY=VALUE`（覆盖配置里的开关，`KEY=` 删除）、
`-Patch <cheat.json>`（etaHEN 格式补丁）、`-PresentMode`、`-Vblank`、`-Game <游戏目录>`（记在 `game-path.txt`，
都找不到时弹出选择框）、`-Affinity <十六进制掩码>`。

- 依赖：VS 2022（C++ 工作负载）、LLVM 19.1.7（`winget install LLVM.LLVM --version 19.1.7`）、Vulkan SDK（glslangValidator）。
  LLVM 23.1.2 编译 `agc.cpp` 时编译器自身崩溃，不要用。
- 原 CPU 4、5 两个 P 核不稳定（clang 随机崩溃、系统蓝屏 0x20001），09-30 起在 BIOS 里禁用：现为 6 P 核（0–5）+ 16 E 核（6–21），
  构建与运行不再需要亲和性掩码（`build-windows.cmd` 只在设了 `KYTY_BUILD_AFFINITY` 时才绑核）。
- 渲染线程限定在 P 核 1–5（配置的 `KYTY_RECORDING_CPUS`，非独占）：比自由调度快约 2.5%。**不要像 Linux 那样独占 CPU 0**：
  Windows 的中断/DPC 集中在 CPU 0，帧率直接减半；用 CPU Set 把其他线程赶出某个 P 核也会大幅变慢。
- 给其他电脑：`.\package-windows.ps1` 在 `_Build\windows-portable` 编 x86-64-v3 版（`-DKYTY_MARCH=x86-64-v3`；
  `-march=native` 会用到本机的 GFNI，别的 CPU 上直接非法指令），再装配 `_Build\portable\KytyPS5`（约 135 MB）：
  exe、VC++ 运行库 DLL（随包，不用装）、libwinpthread、`srt-aot.dll`、`run-windows.ps1`、双击用的 `run.cmd`，
  以及去掉本机 CPU 设置和 Linux 路径的 `launch.json`（不绑核）；另有预编译程序、`seeds.seeds`、`precompile.cmd` 和
  给测试者的 `README.md`（源文件 `docs/PORTABLE-README.md`）。不含游戏、存档、着色器缓存（按 GPU+驱动区分）和 Streamline。
  窗口比屏幕大时模拟器按比例缩进可用区域。
- 着色器准备：启动器每次先跑 `kyty_shader_precompile --status`（约 1 秒，报告预取输入和静态管线缓存是否属于当前
  GPU+驱动），预取输入缺失或过期时自动生成（首次运行、更新驱动后）。`run.cmd` 带 `-Prompt`：静态缓存没做或游戏版本不是
  PPSA01341 01.007.000 时先弹窗（先预编译 / 直接开始 / 退出，可勾选不再提示，记在 `no-precompile-prompt.txt`）。
- PGO：`_Build\pgo\windows\kyty.profdata` 存在时自动使用。重新训练：`KYTY_BUILD_DIR=_Build\windows-pgo-gen`、
  `KYTY_CMAKE_ARGS=-DKYTY_PGO_GENERATE=ON -DKYTY_THIN_LTO=OFF` 编译插桩版，用它进游戏走固定场景，
  live 命令 `pgo <路径>` 导出 profraw（模拟器以 quick_exit 退出，不会自动写出），`llvm-profdata merge` 后 clean 重编正式版。
- SRT AOT：`python tools\local\compile-srt-aot-windows.py <Linux 库目录>` 用同一批 unit-*.cpp 编出 `srt-aot.dll`；
  启动器把配置里的 `.so` 路径自动换成同目录的 `.dll`（约 +1.5%）。
- 驱动管线缓存与预热：启动器用 exe 的 SHA256 设置 `KYTY_DRIVER_CACHE_KEY`，缓存在 `_PipelineCache\local\<sha>\`
  （约 190 MB，每次重编换目录，旧目录可删）。预热输入按"驱动版本+UUID"分目录，当前签名没有文件时自动接管同一游戏、
  同一 GPU 最新的文件（Linux 录的 2860 输入 / 2756 管线即这样迁移）。管线预热按 `KYTY_SHADER_WARMUP_THREADS` 并行。

### Windows 专有问题与修复

- 游戏的 SysV red zone：Windows 在出错线程的栈上分发异常（写保护跟踪每帧数百次），会踩掉 rsp 下方 128 字节。
  `--redzone`（默认开）改写游戏里用 red zone 的指令；补丁器只覆盖 `.eh_frame` 里有记录的函数，仍观察到约 1/20 次运行
  有偶发崩溃（`eboot.bin+0x1c5e2ce` 读 `[rsp-0x68]` 得到 0；另有一次 `unknown sh reg`），尚未解决。
- 开场/菜单卡死（窗口未响应）：NVIDIA Windows 驱动在 `vkQueuePresentKHR` 里阻塞于内核时持有设备级锁；
  若 GPU 上有"先等待后 signal"（等一个还在录制线程队列里的 tick），就与录制线程互相等待。已修：
  回读拷贝引擎（`KYTY_READBACK_QUEUE`）的提交改由录制线程按顺序发出；present 前等推迟提交交给驱动、
  CPU 等 blit 完成；present 走独立队列；present 频率不超过显示器刷新率（开场 200+ FPS 时丢弃多余帧）。
  修复后开场循环 10 分钟浸泡测试无卡死。
- SDL2 默认开启文本输入，使日文/中文输入法吞掉字母键（J=Cross、WASD）：窗口创建后关闭文本输入。
- 进程改为按显示器感知 DPI（`SDL_HINT_WINDOWS_DPI_AWARENESS=permonitorv2`）：200% 缩放下全屏交换链是真正的
  5120×2160，窗口尺寸按物理像素计；此前是 2560×1080 由 DWM 放大，画面发虚。
- 进程崩溃后可能残留一个无法结束的 `kyty_emulator.exe`（状态 Unknown、1 个线程，驱动在等 GPU 队列），
  占用 exe 文件；重编前把旧 exe 改名即可。
- 显存小于 32 GB 的显卡会启动纹理/缓冲回收（GC 阈值按显存预算算：16 GB 卡约 3 GiB 起，24 GB 卡在开阔场景也会），
  RTX 5090 从不启动，所以此前的问题只在别人的机器上出现（10-01 修）：GC 下载脏缓冲后立即释放，而回读队列只等
  最后一次写入 → 8 GB 时读档就 `VK_ERROR_DEVICE_LOST`；GC 的"年龄"按提交计（本游戏每帧约 40 个），几帧没用的
  资源就被删掉又重建；只经 BDA 页表读的缓冲从不进 LRU，每帧被删、缺页又建回（8 GB 时每帧 227 次缓冲注册，
  19.7 fps）。现在 GC 每帧一次，有 BDA 着色器的帧不回收缓冲：8 GB 模拟下 45.3–45.6 fps、16 GB 下 45.2–45.9 fps
  （同样有后台负载时 8 GB 38.3–39.6、不限显存 37.6–39.3）。测试开关 `KYTY_VRAM_LIMIT_MB=<n>`：显存堆限制为 n MiB（VMA heap size limit）。
- 原生 XPR 记录的重定位（10-02，`KYTY_NATIVE_XPR_RELOCATE=1`，`run-windows.ps1` 默认开）：直接绘制（DRAW_INDEX_2）的对象
  每帧拿到指向新一份常量表的 s0 指针，记录的键每帧都是新的，直接绘制几乎从不走原生路径（Latria 每帧 4300 次只命中约 120）。
  现在只被 SRT 当指针基址读、或根本没人读的用户数据字离开键，记录按帧内第 n 次出现区分，读表时从新基址读相同偏移；表里再
  指向别处的指针（只当指针用的结构字）变化时，经它读的内容跟着移动，不再整份重新求值。Latria 直接绘制命中约 3100/帧，
  同进程 ABBA：Latria 33.1→36.9 fps，1-1 30.7→31.8，4-1 40.5→41.2。顺带修复：渲染目标地址上出现同一底层的新图像后，
  原生绘制状态仍画进旧图像（阴影贴图，原先极少触发）；记录重新求值只读已映射内存（陈旧指针曾在启动时读 0x10 崩溃）。
  走动时同一着色器对的物体不断进出视野，"第 n 次出现"每帧对应到别的物体：Latria 行走时大多数重定位记录每帧重新求值或
  补描述符（约占渲染线程 15%）。现在键里加上该次绘制的首个索引地址（网格），第 n 次只在同一网格内计数。
  分开启动交替 A/B：Latria 行走 26.75→29.75 fps（站立 36.25→36.8），1-1 持平。分开启动的场景会不同（有的出生点会被怪物/
  飞龙攻击），同进程交替（来回巡逻同一段路，4 对窗口）：Latria 34.62→35.42 fps，渲染线程 26.80→26.15 ms/帧。
- 走动时偶发的 0.1–0.4 s 卡顿（10-02）：每次 dispatch/绘制都读着色器头部的声明哈希，直接读游戏内存；走动中流入的
  新内容让着色器代码所在页上同时有 GPU 写过的数据（整页被读保护），一读就缺页、同步回读，渲染线程等 GPU 做完前面的
  全部工作（Latria 行走 profile 里占渲染线程 6%，一次 413 ms）。现在和代码哈希一样：读的字节本身不是 GPU 写的就从 backing 读。
- 走进新区域时的卡顿（10-02，`KYTY_NATIVE_XPR_STORE_BUDGET=256`，`run-windows.ps1` 默认开）：走动时每帧有上千个新物体/LOD
  第二次出现、要存原生 XPR 记录（每个约 16 µs），一帧全存就是 60–90 ms 的帧。现在每帧最多存 256 个，其余的下一帧再要。
  分开启动交替各 4 次：Latria 行走 1% low 14.1→16.4 fps、p99 62→56 ms、最长帧 86→70 ms；1-1 p99 56→53 ms、最长帧
  74→65 ms；平均 fps 持平。
- 偶发的 0.2–1.8 s 卡顿（10-02）：游戏读 GPU 写过的内存时，512 KiB 回读窗口里 GPU 写过的范围可能碎成上千段，每段一次
  vkCmdCopyBuffer，一帧几十次回读就是几秒的驱动调用（Latria 行走 profile：1.8 s 的一帧 77% 在回读）。现在相距不到 64 KiB
  的段合成一次复制（中间的字节也拷，但不写回），写回客机内存仍按原来的每段范围。

### 性能（固定场景：基准存档 Continue → HUD 后前进 10 s → 静止，2560×1440 窗口）

| 版本 | FPS | 渲染线程 CPU/帧 |
|---|---:|---:|
| 初始移植（/O2，无预热/缓存） | 30.8 | 27.4–28.2 ms |
| + profiler 内联判断、录制线程短自旋、自旋锁 TTAS | 31.2 | 26.7–27.8 ms |
| + `-O3 -march=native` + ThinLTO | 32.5 | 25.9–26.4 ms |
| + 渲染线程限定 P 核（不含 CPU 0） | 33.4 | 25.0–25.4 ms |
| + PGO + SRT AOT DLL | 36.2 | 22.0–22.4 ms |
| 最终（含上述卡死修复，PGO 重新训练） | **35.6–35.75** | 21.4–21.9 ms |

渲染线程忙碌约 77–80%：每帧约 4 ms 在等游戏提交下一帧（游戏要等上一帧 GPU 工作完成），GPU 每帧忙约 16 ms。
复测：`tools\local\windows\bench-run.ps1 -Label <名字>`（每次重装基准存档，截图识别 HUD，live `measure`）；
先 `python tools\local\bench-windows.py prepare` 备份用户存档，结束后 `restore`（逐文件 SHA256 核对）。

### 21:9 / 5K

- `-AspectFit`：按比例居中显示。5120×2160 屏上 3840×2160 画面 1:1 显示、两侧黑边，无缩放。
- 实验补丁 `tools\local\patches\aspect-21x9-experimental.json`：把渲染初始化里的 16:9 常量（`0x90ae7f`）改为 64:27，
  配合拉伸显示（不加 `-AspectFit`）得到正确比例的 21:9 水平视野扩展（已截图对比验证）。代价：HUD 横向拉伸 1.33 倍，
  横向清晰度为 3840/5120；视野更宽、绘制更多，帧率未测。原生 5120×2160 需要改几十处渲染目标尺寸与 UI，属于长期逆向工作。

- live 命令：`measure`、`trace`、`census`、`pin`、`cpuset`、`set <符号> <值>`（经 map 文件换算地址）；
  `prof`/`profw` 为挂起采样（墙钟），`profp` 不支持。
