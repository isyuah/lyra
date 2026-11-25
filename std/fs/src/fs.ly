import std.io.IoError
import std.io.IoErrorKind
pub import std.io.TempFile
pub import std.io.createTempFile

extern fn lyraFsPathJoin(base: String, segment: String): String;
extern fn lyraFsListDir(path: String): CPtr;
extern fn lyraFsDirStatus(directory: CPtr): Int;
extern fn lyraFsDirCount(directory: CPtr): Int;
extern fn lyraFsDirName(directory: CPtr, index: Int): String;
extern fn lyraFsMkdirAll(path: String): Int;
extern fn lyraFsRemoveFile(path: String): Int;
extern fn lyraFsAtomicWrite(path: String, content: Bytes): Int;

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
