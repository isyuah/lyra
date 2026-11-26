# std.io：文本、字节和文件句柄

- `readFile(path): Result<String, IoError>` 验证整文件为 UTF-8；`readBytes(path): Result<Bytes, IoError>` 原样读取。空文件成功返回空文本或空字节。
- `writeFile(path, text)`、`writeBytes(path, bytes)` 创建或覆盖文件，返回 `Result<Unit, IoError>`；报错包含写入和关闭失败。`writeError(text)` 原样写入并刷新标准错误，也返回 `Result<Unit, IoError>`。
- `IoError.kind` 区分 `System(code)`、`Closed`、`Busy`、`Invalid`、`InvalidUtf8` 和 `LineTooLong`；`operation` 说明失败的操作，`code()` 仅对 `System` 有值，不保证跨平台相同。调用方可读取 `message()`，或经 `?` 传播到共同 `Error`。
- 文件正文与重定向输出保留内嵌 NUL；文件路径拒绝 NUL。Windows 使用 UTF-16 原生文件路径，Lyra API 仍用 UTF-8。目录、路径和临时文件见 [`std.fs`](../fs/README.md)，命令行参数见 `std.os.args()`。

## 有界读行

`readLine(maxBytes: Int = 1048576): Result<String?, IoError>` 读标准输入。`LineReader.stdin(maxBytes)` 和 `LineReader.fromFile(file, maxBytes)` 创建保存该限制的读取器，随后用 `reader.readLine()` 读取。读取器不接管或复制输入；多个读取器共享标准输入或原 `File` 的当前位置。保存的限制必须为正数。

`Ok(Some(text))` 表示一行，包括空行；`Ok(None)` 表示 EOF。LF 与 CRLF 终止符不计入行长，其他 CR 保留。行长按 UTF-8 字节计；`InvalidUtf8` 和 `LineTooLong` 都会消费完整坏行，下一次读取从新行开始。最后一行可以没有换行。系统读取失败仍是 `Err(System(...))`。

## 句柄

- `File.openRead(path)`、`File.openWrite(path)`、`File.openAppend(path)` 都返回 `Result<File, IoError>`。写模式创建或截断；追加模式创建或保留，写入总是追加到末尾。
- `file.read(maxBytes: Int = 65536)` 单次最多读 64 KiB；`Ok(Some(bytes))` 含非空片段，`Ok(None)` 为 EOF。`file.write(bytes)` 可多次调用；`file.flush()` 刷新缓冲，可处理失败。
- `file.seek(offset: Int, origin: SeekOrigin = SeekOrigin.Start): Result<Int, IoError>` 用 64 位字节偏移定位并返回新位置。`Current` 和 `End` 分别从当前位置和末尾开始。追加模式即使定位，下一次写入也仍在末尾。
- `file.close()` 是同步清理，可用 `defer file.close()`；别名共享关闭，闲置时重复关闭成功。并行操作占用同一文件时返回 `IoErrorKind.Busy`；此时 `close` 抛 `std.io.Busy` 而不关闭，其他关闭失败抛 `std.io.CloseError`。I/O 都是同步的，服务端可以通过 `blocking` 隔离等待。
- `File.withRead(path) { file => ... }`、`File.withWrite(path) { file => ... }` 和 `File.withAppend(path) { file => ... }` 在回调退出时关闭句柄，包括返回 `Err` 或抛异常的路径；异步调用时把整个作用域放进一个 `blocking` 块。

```lyra
import std.io.{File}
import std.io.{IoError}

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
