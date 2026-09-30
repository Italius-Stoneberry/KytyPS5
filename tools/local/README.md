# 本地工具

日常启动与必要资产见 [当前版本](../../docs/CURRENT-PLAYABLE.md)，所有实验结论见 [实验简记](../../docs/EXPERIMENTS.md)，测试流程见 [性能复测](../../docs/BENCHMARKING.md)。

## 日常运行和测量

- 项目根目录 `./run.sh`，或 `play-demons-souls.py --2k`：同一正式配置。后者支持运行日志、录像和 shader 预热选项。
- `benchmark-entry.py`：备份用户存档、加载固定基线、正常 Continue、定位、静止测量，正常退出后恢复原存档。
- `quick-benchmark-game.py`：已有进程的短测/截图；默认无镜头输入。
- `switch-control.py`、`abba-switch.py`、`abba-cpu.py`：身份/指纹绑定的同进程开关与 CPU/帧对照。
- `debug-run.py`、`debug_cpu_policy.py`、`demons-souls-smoke.py`、`close-game-window.py`：进程、绑核、菜单及正常关闭。
- `game_recording.py`、`recording_hud.py`、`playtest_log.py`：录像与运行日志。

## 构建和诊断

- `compile-srt-aot.py`：从生成的计划重建 AOT，ABI 头为 `src/local/SrtAotAbi.h`。
- `extract-game-shaders.py`、`capture-srt-plans.py`、`shader_precompile_audit.py`：资源提取、计划导出与预热审计。
- [static-precompile](static-precompile/README.md)：从游戏文件静态收集全部 shader/管线，独立程序（`precompile-windows.ps1`，多进程）预编译进跨构建保留的静态管线缓存；含与录制缓存的覆盖率对比。
- `profile-game.py`、`perf-code-maps.py` 和各 `*-report.py`：按需诊断；不能把插桩 FPS 当成净收益。
- `stage-candidate.py`：为独立候选生成配置，避免覆盖正式版本。
- `run-reviewed.py`、`benchmark-build.py`：高级独立构建入口；需显式提供自己的二进制/收据，不是当前日常入口。
- [PGO](PGO.md)：当前 profile 输入与重新训练的注意事项。

CMake 与测试引用的本地源码（vulkan-recording、native-resource 等）都在 `src/local/`，可选实验默认关闭。本目录原有的 shader-preparation、prepared-draw、render-cost/playtest 等旧实验源码、`src/local` 文件的旧副本、`*.cmake` 片段及其测试源码，以及 `build-pbr-full-overlay.py`，都不再被引用，已于 2026-09-28 移除。已结束的 PT/SRT 捕获回放专用工具、旧 checkpoint 字节补丁工具及大体积运行产物已移除。

新实验只在实验简记追加一行。保留可复用工具，及时删除旧输出目录；不要删除正在使用的进程锁、正式库、存档或构建输入。
