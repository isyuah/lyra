extern fn lyraFsCreateTempFile(directory: String, prefix: String): CPtr;
extern fn lyraFsTempStatus(result: CPtr): Int;
extern fn lyraFsTempPath(result: CPtr): String;
extern fn lyraFsTempHandle(result: CPtr): CPtr;

pub struct TempFile(pub val path: String, pub val file: File)

pub fn createTempFile(directory: String, prefix: String = "lyra-"): Result<TempFile, IoError> {
    val result = lyraFsCreateTempFile(directory, prefix)
    val status = lyraFsTempStatus(result)
    if status == 0 { Ok(TempFile(lyraFsTempPath(result), File(lyraFsTempHandle(result)))) }
    else { Err(ioError("createTempFile", status)) }
}
