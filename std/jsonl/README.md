# `std.jsonl`

`jsonlReader` 从 `LineReader` 逐行读取，可传入 `JsonCodec<T>` 或自定义解码函数 `(String) -> Result<T,String>`。它不缓存整个输入。

```lyra
import std.io.{IoError}
import std.io.{LineReader}
import std.json.{jsonCodecInt}
import std.jsonl.{jsonlReader}

fn readNumbers(): Result<Unit, IoError> {
    val rows = jsonlReader(LineReader.stdin()?, jsonCodecInt())
    while let Some(row) = rows.next()? {
        match row.value {
            Ok(number) => print(number),
            Err(error) => print("line ${row.lineNumber}: ${error.message()}"),
        }
    }
    Ok({})
}
```

`next()` 用 `Ok(None)` 表示 EOF、用外层 `Err(IoError)` 表示读流失败。解析或解码失败、非法 UTF-8、超出行长度限制属于该行的错误，仍有从 1 开始的物理行号；可以继续读取下一行。空行也交给解码器；不自动忽略。使用 `LineReader.fromFile` 时文件仍由调用方关闭。
