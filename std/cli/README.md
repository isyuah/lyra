# `std.cli`

```lyra
import std.cli.Command
import std.cli.parseArgs

val spec = Command("stats")
    .option("--input", required: true)
    .flag("--verbose")
val result = parseArgs(spec)
```

`parseArgs(spec)` 读取进程参数（不含程序名），`parseArgs(spec, arguments)` 使用显式列表。`Run(parsed)` 可通过 `value("--input")`、`flag("--verbose")` 和 `positionals` 取值；未给出的可选值为 `None`，未给出的标志为 `false`。`--help` 返回 `Help(text)`，不检查必填项，由应用决定输出和退出状态。

- 值参数接受 `--name value` 或 `--name=value`。值若以 `--` 开头，使用 `=` 保留它；`--name=` 表示显式空值。
- `--` 停止解析选项，单独的 `-` 是位置参数。选项可以和位置参数交错；保留位置参数的顺序。
- 仅识别已注册的长选项，选项名区分大小写。重复传参、缺值、未知选项和未满足的必填项返回 `CliError`；库不自行输出或退出。
