# 性能基准

在仓库根目录，使用已构建或已安装的 Lyra SDK 运行。脚本使用 Python 3.11 以上版本，应用对照另需本机 Go。Lyra SDK 自带原生编译和链接工具。

## 应用对照

```powershell
python tests/benchmarks/applications.py build/windows-release/bin/lyra.exe --output build/benchmarks/windows-applications.json
```

```sh
python3 tests/benchmarks/applications.py build/release/bin/lyra --output build/benchmarks/linux-applications.json
```

Lyra 和 Go 分别生成本机 Release 应用，以相同的 64 位整数结果核对 CLI；HTTP 应用使用相同的路径、状态码、JSON 正文和 HTTP/1 长连接。两端都限制同时处理的 handler 为 64。负载由同一个 Python asyncio 客户端产生，每条连接顺序发送请求，校验响应状态、长度与正文。

- 构建使用包含 200 个函数的程序，分别记录冷缓存、无修改、函数体修改及再次无修改的耗时。临时 `LYRA_CACHE` 和 `GOCACHE` 相互隔离，Go 冷缓存阶段包含标准库编译。`--functions` 可以扩大输入规模。
- 启动测量包含创建进程、输出一个已验证结果和退出，以预热后样本报告 p50/p95。默认记录 10 次，`--runs` 可调整。
- HTTP 默认测 1、16、64 条连接，各启动三个全新的服务进程。每条连接先预热 10 次，再记录 200 次请求。报告各轮吞吐、单请求延迟 p50/p95，以及吞吐的样本分布。
- 内存记录服务接入负载前后的 RSS 和峰值 RSS。Windows 读取进程 Working Set，Linux 读取 `/proc/<pid>/status`。可执行文件大小单独记录。
- JSON 报告包含工具版本、编译器 SHA-256、Python 版本、平台和负载参数。脚本正常结束时通过应用的停机入口回收服务。

该负载的并发连接数固定，结果包含 Python 客户端和 loopback 开销。比较时保留同一台机器、相同参数和同一客户端，独立运行各平台基准，保存完整 JSON。需要研究最大吞吐时，应另用独立压测机提高到达率并观察服务端与客户端 CPU。

## 多包 LSP

```powershell
python tests/benchmarks/lsp.py build/windows-release/bin/lyra.exe --output build/benchmarks/windows-lsp.json
```

```sh
python3 tests/benchmarks/lsp.py build/release/bin/lyra --packages 2 8 24 --functions 40 --runs 10
```

脚本创建有 2、8、24 个本地依赖包的应用，每个依赖公开 40 个函数。编辑器同时打开调用者和一个依赖文件，连续发送未保存的函数体修改，测量 `didChange` 到对应文档版本诊断发布的 p50/p95，计入服务端防抖。开始和结束时都用 CLI 全量检查确认程序有效；诊断出现错误、版本不符或服务退出会使测量失败。`--packages` 和 `--functions` 控制项目规模，完整检查耗时一并写入 JSON。

## 并行计算

```powershell
python tests/benchmarks/parallel_for.py build/windows-release/bin/lyra.exe
```

```sh
python3 tests/benchmarks/parallel_for.py build/release/bin/lyra --workers 1 2 4 --warmups 2 --runs 5
```

脚本在临时目录中编译一次 Release 应用，同一可执行文件依次在多个进程运行，每个进程只变更 `LYRA_PARALLEL_WORKERS`。每次运行用试除法统计 `[2, 2000000)` 中的素数，分块负责不重叠的区间，只在每块结束时用原子操作合并结果。每轮（包括预热）必须得到 **148,933** 个素数、总和 **142,913,828,922**，并核对进程实际报告的 Worker 容量。

使用随 SDK 链接的 libuv 单调时钟，测量从启动 `parallelFor` 到任务组完成的墙钟时间，包含调度与结果归并，不包含编译、进程启动、输入和打印。每个进程先丢弃预热样本，正式样本复用已启动的 Worker 池。输出中位数、最值及相对于第一组的加速比；`--workers 1 2 4 8` 可扩大对照范围。建议在同一台机器上关闭其他高占用任务后比较，结果会受可用 CPU、系统负载和频率调节影响。该脚本不注册到 CTest，也不设固定性能门槛。
