# std.io：文本和文件

`argCount()` 不含程序名，`arg(0)` 是第一个用户实参。`arg`、`readLine` 和 `readFile` 都返回 `TextRead`：`.ok()` 时 `.value` 为读到的文本，可能为空；`.eof()` 只表示标准输入已读完；其他失败的 `.status` 是本机 C Runtime 的正错误码。`.invalidUtf8()` 表示输入不能解码为 UTF-8；`readLine` 已消费该行，可以继续读下一行。逐行读取去掉 LF 或 CRLF，不去掉最后一个单独的 CR。Windows 的程序实参与文件路径走 UTF-16 系统接口，Lyra 中仍用 UTF-8。文件内容和重定向输出保留内嵌 NUL；路径拒绝 NUL。`writeFile` 以覆盖模式写入，`writeError` 原样写到标准错误，两者成功返回 `0`。

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

## 原始字节

- `readBytes(path): Result<Bytes, IoError>` 读整个文件。空文件返回 `Ok(b"")`；失败返回 `Err`。`writeBytes(path, bytes): Result<Unit, IoError>` 创建或覆盖文件，返回前检查缓冲写入与关闭失败。
- `File.openRead(path)` / `File.openWrite(path)` 返回 `Result<File, IoError>`。后者创建或覆盖。`file.read(maxBytes: Int = 65536)` 每次读取最多 `maxBytes` 字节（单次上限 64 KiB）：`Ok(Some(bytes))` 表示读到非空数据，`Ok(None)` 表示 EOF，`Err` 表示操作失败。长度必须为正。`file.write(bytes)` 可多次调用；`file.flush()` 提交缓冲写入，可检查操作失败。
- 字节 API 保留原始文件内容，包括 NUL、CRLF 和无效 UTF-8。文件路径仍是 `String`。`IoError` 保留操作名、错误类别和可选的本机错误码；可用 `message()` 显示，或经 `?` 传播到共同 `Error`。
- 所有操作是同步的。服务端用 `blocking` 隔离可能阻塞的文件操作。`close()` 是同步清理，可用 `defer file.close()`。别名共享关闭；闲置时重复关闭成功，关闭后读写返回 `Closed`，读写或关闭正使用句柄时抛 `std.io.Busy` 而不关闭。同一个文件对象上的并行读写返回 `IoErrorKind.Busy`，其他关闭错误抛 `std.io.CloseError`。

```lyra
import std.io.File
import std.io.IoError

fn copyFile(source: String, destination: String): Result<Unit, IoError> {
    val input = File.openRead(source)?
    defer input.close()
    val output = File.openWrite(destination)?
    defer output.close()
    while true {
        match input.read()? {
            None => break,
            Some(chunk) => { output.write(chunk)?; },
        }
    }
    output.flush()
}
```
