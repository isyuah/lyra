# std.io：读写 UTF-8 文本

`argCount()` 不含程序名，`arg(0)` 是第一个用户实参。`arg`、`readLine` 和 `readFile` 都返回 `TextRead`：`.ok()` 时 `.value` 为读到的文本，可能为空；`.eof()` 只表示标准输入已读完；其他失败的 `.status` 是正的 Linux `errno`。非法 UTF-8 视为读取错误，而不替换字节。逐行读取去掉 LF 或 CRLF，不去掉最后一个单独的 CR。文件内容和输出保留内嵌 NUL；路径拒绝 NUL。`writeFile` 以覆盖模式写入，`writeError` 原样写到标准错误，两者成功返回 `0`。

把下面的 `main.ly` 放在应用包的 `src/` 中，编译后执行 `./app note.txt`；输入一行中文并回车即可写入文件。

```text
import std.io.arg
import std.io.assert
import std.io.readLine
import std.io.writeFile

fn main(): Unit {
    val path = arg(0)
    assert(path.ok(), "请给一个输出文件名")
    val line = readLine()
    if line.eof() { return }
    assert(line.ok(), "输入不是有效的 UTF-8 文本")
    assert(writeFile(path.value, line.value + "\n") == 0, "写入失败")
}
```

`assert` 在 Debug、Release 都生效：条件不成立时把消息写到标准错误并以非零状态退出。它适合测试和简单 CLI；服务程序可以检查状态并自行处理失败。
