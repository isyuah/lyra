extern fn lyraFsCreateTempFile(directory: String, prefix: String): CPtr;
extern fn lyraFsTempStatus(result: CPtr): Int;
extern fn lyraFsTempPath(result: CPtr): String;
extern fn lyraFsTempHandle(result: CPtr): CPtr;

/// 已独占创建并打开的临时文件。调用方负责关闭句柄和删除路径。
pub struct TempFile(
    /// 实际创建的路径，可用于后续清理。
    pub val path: String,
    /// 已打开的可写句柄，无需再打开路径。
    pub val file: File
)

/// 在给定目录独占创建临时文件；`prefix` 是文件名开头，不能含路径分隔符、冒号或 NUL。
pub fn createTempFile(directory: String, prefix: String = "lyra-"): Result<TempFile, IoError> {
    val result = lyraFsCreateTempFile(directory, prefix)
    val status = lyraFsTempStatus(result)
    if status == 0 { Ok(TempFile(lyraFsTempPath(result), File(lyraFsTempHandle(result)))) }
    else { Err(ioError("createTempFile", status)) }
}
