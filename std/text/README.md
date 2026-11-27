# std.text

显式导入 `std.text.StringOps` 后可在 `String` 上使用文本操作。位置从零开始，按 Unicode 标量计数，不按 UTF-8 字节或字素计数；所有操作都保留内嵌 NUL，不做 Unicode 归一化。

```lyra
import std.text.{StringOps}

fn main(): Unit {
    val fields = "  Ada,ff,  ".trim().split(",")
    print(fields[0])
    print(match fields[1].parseInt(radix: 16) { Ok(value) => value, Err(_) => 0 })
}
```

- `substring(start, end)` 取半开标量区间；省略 `end` 则取到末尾。`slice(range)` 接受 `..<` 和 `..`，与列表切片的边界一致。空切片合法；负数、倒序或越界抛异常。
- `startsWith(prefix)` 和 `endsWith(suffix)` 区分大小写；空前缀或后缀总是匹配。
- `contains(fragment)` 区分大小写；空片段总是匹配。`replace(fragment, replacement)` 从左到右替换全部非重叠的字面匹配，不重复搜索替换产物。空片段在每个 Unicode 标量边界插入，包括首尾；空文本只插入一次。
- `indexOf(fragment): Int?` 返回首个完整匹配的标量下标，找不到时是 `None`，空片段返回 `Some(0)`。
- `split(separator)` 从左到右按非重叠的字面分隔符分割，保留首尾和相邻分隔符间的空段。空字符串分割得到单个空段；空分隔符抛异常。
- `trim()` 去掉两端具有 Unicode `White_Space` 属性的标量，不改变内部空白。
- `parseInt(radix: Int = 10): Result<Int, ParseIntError>` 接受 `2` 至 `36` 进制和可选的前导 `+`／`-`，按 ASCII 数字和英文字母解析完整输入。空值或只有符号返回 `Empty`，非法基数返回 `InvalidRadix`，非法数位返回 `InvalidDigit`（零基位置），超出有符号 64 位范围返回 `Overflow`。不会忽略空格、进制前缀或分隔符。
- `parseFloat(): Result<Float,ParseFloatError>` 接受可带符号和十进制指数的有限浮点数，使用 `.` 作为小数点，不受系统区域设置影响。必须解析完整输入；空串返回 `Empty`，空格、十六进制、`NaN`、无穷大与其他非法格式返回 `Invalid`，超出表示范围返回 `OutOfRange`。

## Bytes

`Bytes` 的以下操作无需导入：`bytes.slice(range)` 按字节切片，切片越界抛 `BoundsError`；`left + right` 拼接原始字节；`bytes.toHex()` 编码成小写、无前缀的十六进制；`Bytes.fromHex(text): Bytes?` 接受大小写，每两个十六进制字符解码一个字节，非法字符或奇数长度返回 `None`。空文本解码成空字节串。切片和拼接不要求 UTF-8 有效。
