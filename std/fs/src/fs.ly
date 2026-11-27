import std.io.{IoError, IoErrorKind}
pub import std.io.{TempFile, createTempFile}

extern fn lyraFsPathJoin(base: String, segment: String): String;
extern fn lyraFsListDir(path: String): CPtr;
extern fn lyraFsDirStatus(directory: CPtr): Int;
extern fn lyraFsDirCount(directory: CPtr): Int;
extern fn lyraFsDirName(directory: CPtr, index: Int): String;
extern fn lyraFsMkdirAll(path: String): Int;
extern fn lyraFsRemoveFile(path: String): Int;
extern fn lyraFsAtomicWrite(path: String, content: Bytes): Int;
extern fn lyraFsStat(path: String, follow: Bool): CPtr;
extern fn lyraFsInfoStatus(info: CPtr): Int;
extern fn lyraFsInfoKind(info: CPtr): Int;
extern fn lyraFsInfoSize(info: CPtr): Int;
extern fn lyraFsInfoModifiedSeconds(info: CPtr): Int;
extern fn lyraFsInfoModifiedNanosecond(info: CPtr): Int;
extern fn lyraFsRename(from: String, to: String, replace: Bool): Int;
extern fn lyraFsCopy(from: String, to: String, replace: Bool): Int;
extern fn lyraFsRemoveDir(path: String): Int;
extern fn lyraFsCwd(): CPtr;
extern fn lyraIoTextStatus(result: CPtr): Int;
extern fn lyraIoTextValue(result: CPtr): String;
extern fn lyraIoInvalidUtf8(status: Int): Bool;

/// `stat` 取样路径时识别的文件系统条目种类。
pub enum FileKind {
    /// 普通文件。
    Regular,
    /// 目录。
    Directory,
    /// 符号链接本身，通过 `followSymlinks: false` 查询。
    Symlink,
    /// 其他条目，例如管道或设备。
    Other,
}

/// 单次 `stat` 读取的元数据快照，不会随之后的文件变化更新。
pub struct FileInfo(
    /// 被查询条目的种类。
    pub val kind: FileKind,
    /// 宿主报告的字节大小；对目录不是子项大小之和。
    pub val size: Int,
    /// 最近修改时间的 Unix 整秒部分。
    pub val modifiedUnixSeconds: Int,
    /// 最近修改时间的秒内纳秒部分，精度取决于文件系统。
    pub val modifiedNanosecond: Int
)

fn fsError(operation: String, status: Int): IoError {
    val kind = match status {
        -4 => IoErrorKind.Invalid,
        -5 => IoErrorKind.InvalidUtf8,
        _ => IoErrorKind.System(status),
    }
    IoError(kind, operation)
}

fn filesystemChange(operation: String, status: Int): Result<Unit, IoError> {
    if status == 0 { Ok({}) } else { Err(fsError(operation, status)) }
}

/// 用本机分隔符拼接非空片段，绝对路径重置前缀；不解析 `.`、`..` 或符号链接。
pub fn pathJoin(parts: String...): String {
    var result = ""
    for part in parts { result = lyraFsPathJoin(result, part) }
    result
}

/// 列出目录的直接子项名称，不含 `.` 和 `..`，不保证顺序；非 UTF-8 名称使整个查询失败。
pub fn listDir(path: String): Result<List<String>, IoError> {
    val directory = lyraFsListDir(path)
    val status = lyraFsDirStatus(directory)
    if status != 0 { return Err(fsError("listDir", status)) }
    val names: List<String> = []
    for index in 0..<lyraFsDirCount(directory) { names.add(lyraFsDirName(directory, index)) }
    Ok(names)
}

/// 创建目录和缺少的父目录；目标已是目录时成功，遇到同名文件时报错。
pub fn mkdirAll(path: String): Result<Unit, IoError> {
    filesystemChange("mkdirAll", lyraFsMkdirAll(path))
}

/// 删除单个文件路径，不遍历目录。
pub fn removeFile(path: String): Result<Unit, IoError> {
    filesystemChange("removeFile", lyraFsRemoveFile(path))
}

/// 将同目录临时文件写完后原子替换目标，读者不会看到部分内容；不复制旧元数据，也不承诺掉电持久性。
pub fn atomicWrite(path: String, content: Bytes): Result<Unit, IoError> {
    filesystemChange("atomicWrite", lyraFsAtomicWrite(path, content))
}

/// 读取路径元数据；路径不存在为 `Ok(None)`，权限等系统失败仍为 `Err`。可选择不跟随符号链接。
pub fn stat(path: String, followSymlinks: Bool = true): Result<FileInfo?, IoError> {
    val info = lyraFsStat(path, followSymlinks)
    val status = lyraFsInfoStatus(info)
    if status != 0 { return Err(fsError("stat", status)) }
    val kind = match lyraFsInfoKind(info) {
        0 => return Ok(None),
        1 => FileKind.Regular,
        2 => FileKind.Directory,
        3 => FileKind.Symlink,
        _ => FileKind.Other,
    }
    Ok(Some(FileInfo(kind, lyraFsInfoSize(info), lyraFsInfoModifiedSeconds(info), lyraFsInfoModifiedNanosecond(info))))
}

/// 判断路径是否存在；仅缺失返回 `false`，保留 `stat` 的查询错误。
pub fn exists(path: String, followSymlinks: Bool = true): Result<Bool, IoError> {
    Ok(stat(path, followSymlinks)? != None)
}

/// 判断路径是否为目录；缺失或为其他种类返回 `false`，查询失败返回错误。
pub fn isDir(path: String, followSymlinks: Bool = true): Result<Bool, IoError> {
    Ok(match stat(path, followSymlinks)? { Some(info) => info.kind == FileKind.Directory, None => false })
}

/// 在同一文件系统内原子重命名，默认不覆盖已有目标。
pub fn rename(from: String, to: String, replace: Bool = false): Result<Unit, IoError> {
    filesystemChange("rename", lyraFsRename(from, to, replace))
}

/// 复制文件内容，默认独占创建目标；覆盖不保证原子性，也不承诺复制全部元数据。
pub fn copy(from: String, to: String, replace: Bool = false): Result<Unit, IoError> {
    filesystemChange("copy", lyraFsCopy(from, to, replace))
}

/// 删除空目录；非空目录会报错，不递归删除。
pub fn removeDir(path: String): Result<Unit, IoError> {
    filesystemChange("removeDir", lyraFsRemoveDir(path))
}

/// 读取进程当前工作目录，不能表示为 UTF-8 时返回错误。
pub fn cwd(): Result<String, IoError> {
    val data = lyraFsCwd()
    val status = lyraIoTextStatus(data)
    if status == 0 { Ok(lyraIoTextValue(data)) }
    else { Err(fsError("cwd", if lyraIoInvalidUtf8(status) { -5 } else { status })) }
}
