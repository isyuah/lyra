# std.fs：路径和目录

- `pathJoin(parts: String...): String` 使用本机分隔符连接非空片段；绝对路径替换已有部分，Windows 上没有盘符的根相对路径沿用已有卷。它只拼接，不解析 `.`、`..` 或符号链接，也不访问文件系统。
- `listDir(path): Result<List<String>, IoError>` 列出直接子项的名称而非完整路径，不含 `.` 和 `..`。顺序不作承诺。不能表示为 UTF-8 的名称使操作失败，不返回截断后的名单。
- `mkdirAll(path): Result<Unit, IoError>` 创建缺少的父目录，已存在的目录成功；同名文件或其他系统失败返回错误。`removeFile(path): Result<Unit, IoError>` 删除单个文件，不遍历目录。
- `stat(path, followSymlinks: true)` 返回 `Result<FileInfo?, IoError>`；路径不存在是 `Ok(None)`，权限等错误仍是 `Err`。信息含 `kind`、`size`、`modifiedUnixSeconds` 和 `modifiedNanosecond`；不跟随链接时可查询链接本身。`exists`、`isDir` 共享这项检查，缺失时都返回 `Ok(false)`。
- `rename(from, to, replace: false)` 是同文件系统的原子重命名，默认使用原生不覆盖操作；`copy(from, to, replace: false)` 复制文件，默认独占创建目标，不承诺原子替换或完整元数据复制。需要覆盖时显式指定 `replace: true`。
- `removeDir(path)` 只删除空目录，不递归。`cwd(): Result<String, IoError>` 读取当前进程的工作目录。
- `createTempFile(directory, prefix: String = "lyra-"): Result<TempFile, IoError>` 在指定目录独占创建文件，返回实际 `path` 和可写的 `file: File`，不关闭后再打开。前缀不可含 `/`、`\`、`:` 或 NUL。通过 `file.close()` 和 `removeFile(path)` 显式清理。
- `atomicWrite(path, bytes): Result<Unit, IoError>` 先写完并关闭同目录的独占临时文件，再通过原生重命名替换目标。读者不会看到部分写入；失败时清理临时文件并保留主错误。不复制原文件元数据，不承诺掉电持久性。

文件系统操作使用 UTF-8 路径，在 Windows 上适配为原生宽字符路径；文件系统输入拒绝空路径和 NUL。操作均为同步，异步服务可使用 `blocking`。
