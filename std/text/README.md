# std.text

显式导入 `std.text.StringOps` 后可在 `String` 上使用文本操作。位置从零开始，按 Unicode 标量计数，不按 UTF-8 字节或字素计数；所有操作都保留内嵌 NUL，不做 Unicode 归一化。

```lyra
import std.text.StringOps

fn main(): Unit {
    val fields = "  Ada,ff,  ".trim().split(",")
    print(fields[0])
    print(match fields[1].parseInt(radix: 16) { Ok(value) => value, Err(_) => 0 })
}
```

- `substring(start, end)` 取半开标量区间；省略 `end` 则取到末尾。`slice(range)` 接受 `..<` 和 `..`，与列表切片的边界一致。空切片合法；负数、倒序或越界抛异常。
- `startsWith(prefix)` 和 `endsWith(suffix)` 区分大小写；空前缀或后缀总是匹配。
- `split(separator)` 从左到右按非重叠的字面分隔符分割，保留首尾和相邻分隔符间的空段。空字符串分割得到单个空段；空分隔符抛异常。
- `trim()` 去掉两端具有 Unicode `White_Space` 属性的标量，不改变内部空白。
- `parseInt(radix: Int = 10): Result<Int, ParseIntError>` 接受 `2` 至 `36` 进制和可选的前导 `+`／`-`，按 ASCII 数字和英文字母解析完整输入。空值或只有符号返回 `Empty`，非法基数返回 `InvalidRadix`，非法数位返回 `InvalidDigit`（零基位置），超出有符号 64 位范围返回 `Overflow`。不会忽略空格、进制前缀或分隔符。
