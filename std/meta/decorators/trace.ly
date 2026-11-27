/// 写在函数或方法前的 `@trace` 在进入函数体时向标准输出写入 `trace: <名称>`。
fn trace(target: String, argument: String): Syntax {
    syntaxAround(syntaxCall(syntaxName("print"), syntaxString("trace: " + target)), syntaxCode(""), syntaxCode(""))
}
