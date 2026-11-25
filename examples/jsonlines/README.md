# 用两个包写一个 JSON Lines CLI

`stats` 用有界 JSONL reader 从标准输入或 `--input` 指定的文件逐行读取，把合法记录的 `delta` 求和，坏行连同行号写到标准错误并继续。正常运行时标准输出只写一份 JSON 报告。`metrics` 是可以单独发布的库，定义输入、报告和 `#derive(Json)` 编解码；应用只导入 `parseLine` 与 `summary`。

从仓库根目录，用已构建的 Lyra 工具链运行：

```sh
lyra test examples/jsonlines/metrics
lyra build examples/jsonlines/stats -o /tmp/lyra-stats
printf '%s\n' '{"station":"东区","delta":7}' 'not json' '{"station":"西区","delta":-2}' | /tmp/lyra-stats
```

标准输出解码为 `{"rows":2,"total":5,"rejected":1}`，标准错误指出第 2 行解析失败。命令行错误只记录行号和错误类别，不回显原始数据。空输入也会产生报告。`delta` 和总和使用有符号 64 位整数；对输入流本身的读取错误，程序以非零状态停止，而不是反复读取同一处错误。

用 `--help` 查看选项。`--config stats.toml` 可设置 `input`（默认 `"-"`，即 stdin）和 `maxLineBytes`（默认 1 MiB）。`LYRA_STATS_INPUT` / `LYRA_STATS_MAX_LINE_BYTES` 可覆盖 TOML，`--input` / `--max-line-bytes` 优先级最高。配置错误退出码为 2，I/O 错误为 1。

本地开发通过 `stats/lyra.json5` 中的 `path: ../metrics` 找到库，无需中心。要验证分发，运行 `ctest --preset dev -R '^jsonlines$' --output-on-failure`：[测试](../../tests/jsonlines_cli.py)把这两个包复制到仓库外，运行库测试、发布到临时中心、锁定和抓取；移走库源码和中心后，用 `--locked --offline` 从已校验缓存构建 Debug 与 Release CLI。
