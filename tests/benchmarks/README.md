# 并行性能基准

在仓库根目录，使用已构建或已安装的 Lyra 编译器运行。脚本只依赖 Python 3；编译应用仍需要各平台的原生链接工具链。Windows 使用 x64 Developer PowerShell。

```powershell
python tests/benchmarks/parallel_for.py build/windows-release/bin/lyra.exe
```

```sh
python3 tests/benchmarks/parallel_for.py build/release/bin/lyra --workers 1 2 4 --warmups 2 --runs 5
```

脚本在临时目录中编译一次 Release 应用，同一可执行文件依次在多个进程运行，每个进程只变更 `LYRA_PARALLEL_WORKERS`。每次运行用试除法统计 `[2, 2000000)` 中的素数，分块负责不重叠的区间，只在每块结束时用原子操作合并结果。每轮（包括预热）必须得到 **148,933** 个素数、总和 **142,913,828,922**，并核对进程实际报告的 Worker 容量。

使用随 SDK 链接的 libuv 单调时钟，测量从启动 `parallelFor` 到任务组完成的墙钟时间，包含调度与结果归并，不包含编译、进程启动、输入和打印。每个进程先丢弃预热样本，正式样本复用已启动的 Worker 池。输出中位数、最值及相对于第一组的加速比；`--workers 1 2 4 8` 可扩大对照范围。建议在同一台机器上关闭其他高占用任务后比较，结果会受可用 CPU、系统负载和频率调节影响。该脚本不注册到 CTest，也不设固定性能门槛。
