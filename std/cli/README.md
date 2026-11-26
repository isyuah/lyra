# `std.cli`

```lyra
import std.cli.{Command}
import std.cli.{parseArgs}

val spec = Command("stats")
    .option("--input", required: true, short: "-i")
    .flag("--verbose", short: "-v")
val result = parseArgs(spec)
```

`parseArgs(spec)` 读取进程参数（不含程序名），`parseArgs(spec, arguments)` 使用显式列表。`Run(parsed)` 可通过 `value("--input")`、`flag("--verbose")` 和 `positionals` 取值；未给出的可选值为 `None`，未给出的标志为 `false`。`-h` / `--help` 返回当前命令的 `Help(text)`，不检查必填项，由应用决定输出和退出状态。

- 值参数接受 `--name value` 或 `--name=value`。以 `--` 或 `-` 加字母开头的值使用 `=`；`--name=` 表示显式空值。短别名是 `-` 加一个 ASCII 字母；`-vi文件` 等价于 `-v -i 文件`。取值选项会消费短组余下文本。长短写法使用同一个长选项键，混用也会检查重复。
- `.subcommand(Command("export").flag("--json"))` 注册子命令，可继续嵌套。命令名之前解析父选项，之后解析子选项，不混用两套命名空间。`Run(parsed)` 的 `name` 是当前命令名，`subcommand` 是选中子命令的解析结果；该结果同样可读取选项并继续下钻。
- `--` 停止解析选项，单独的 `-` 是位置参数。选项可以和位置参数交错；保留位置参数的顺序。
- 选项名和子命令名区分大小写。重复传参、缺值、未知选项或命令和未满足的必填项返回 `CliError`；库不自行输出或退出。
