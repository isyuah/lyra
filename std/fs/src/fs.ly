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

pub enum FileKind { Regular, Directory, Symlink, Other }
pub struct FileInfo(pub val kind: FileKind, pub val size: Int, pub val modifiedUnixSeconds: Int, pub val modifiedNanosecond: Int)

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

pub fn pathJoin(parts: String...): String {
    var result = ""
    for part in parts { result = lyraFsPathJoin(result, part) }
    result
}

pub fn listDir(path: String): Result<List<String>, IoError> {
    val directory = lyraFsListDir(path)
    val status = lyraFsDirStatus(directory)
    if status != 0 { return Err(fsError("listDir", status)) }
    val names: List<String> = []
    for index in 0..<lyraFsDirCount(directory) { names.add(lyraFsDirName(directory, index)) }
    Ok(names)
}

pub fn mkdirAll(path: String): Result<Unit, IoError> {
    filesystemChange("mkdirAll", lyraFsMkdirAll(path))
}

pub fn removeFile(path: String): Result<Unit, IoError> {
    filesystemChange("removeFile", lyraFsRemoveFile(path))
}

pub fn atomicWrite(path: String, content: Bytes): Result<Unit, IoError> {
    filesystemChange("atomicWrite", lyraFsAtomicWrite(path, content))
}

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

pub fn exists(path: String, followSymlinks: Bool = true): Result<Bool, IoError> {
    Ok(stat(path, followSymlinks)? != None)
}

pub fn isDir(path: String, followSymlinks: Bool = true): Result<Bool, IoError> {
    Ok(match stat(path, followSymlinks)? { Some(info) => info.kind == FileKind.Directory, None => false })
}

pub fn rename(from: String, to: String, replace: Bool = false): Result<Unit, IoError> {
    filesystemChange("rename", lyraFsRename(from, to, replace))
}

pub fn copy(from: String, to: String, replace: Bool = false): Result<Unit, IoError> {
    filesystemChange("copy", lyraFsCopy(from, to, replace))
}

pub fn removeDir(path: String): Result<Unit, IoError> {
    filesystemChange("removeDir", lyraFsRemoveDir(path))
}

pub fn cwd(): Result<String, IoError> {
    val data = lyraFsCwd()
    val status = lyraIoTextStatus(data)
    if status == 0 { Ok(lyraIoTextValue(data)) }
    else { Err(fsError("cwd", if lyraIoInvalidUtf8(status) { -5 } else { status })) }
}
