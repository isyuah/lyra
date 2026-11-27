# Option 与 Result

这些方法无需导入，`T?` 与 `Option<T>` 使用同一套方法。直接取带默认值的 Option 用 `value ?? fallback()`；只在 `None` 时求默认值。

| 操作 | `Option<T>` | `Result<T,E>` |
|---|---|---|
| 判断分支 | `isSome()`、`isNone()` | `isOk()`、`isErr()` |
| 变换成功值 | `map { value => ... }` | `map { value => ... }` |
| 串接可能失败的计算 | `flatMap { value => ... }`，回调返回 `Option<U>` | `flatMap { value => ... }`，回调返回 `Result<U,E>` |
| 恢复 | `orElse { ... }`，回调不接收参数、返回 `Option<T>` | `orElse { error => ... }`，回调返回 `Result<T,F>`，可换错误类型 |
| 取带默认值的成功值 | `value ?? default` | `unwrapOr(default)` 按普通实参求值；`unwrapOrElse { error => ... }` 仅在失败时调用 |
| 显式互转 | `toResult { ... }`，仅在 `None` 时构造 `Err` | `toOption()`，丢弃错误，`Ok(None)` 得到 `Some(None)` |

回调只在对应分支运行一次。`map` 保留一层包装，`flatMap` 则直接采用回调返回的包装。`Result.mapErr { error => ... }` 只变换 `Err` 的载荷；`failureInfo()` 仍能访问映射前的错误及历史。若映射得到的 `Error` 已指定原因，原错误保存在附加失败中。

`transpose()` 交换外层和内层。`Option<Result<T,E>>` 中的 `None` 变成 `Ok(None)`，`Some(Ok(value))` 变成 `Ok(Some(value))`，`Some(Err(error))` 变成 `Err(error)`。`Result<Option<T>,E>.transpose()` 做逆向转换，保留失败信息。

```lyra
import std.text.{ParseIntError, StringOps}

fn parseOptionalPort(text: String?): Result<Int?,ParseIntError> {
    text.map { it.parseInt() }.transpose()
}
```

`unwrap()` 和 `expect(message)` 取 `Some` / `Ok` 的值，失败时在调用处抛出 `UnwrapError` 并照常执行 `defer`。用在测试或内部不变量上，预期内的失败使用 `?`、`orElse` 或 `match`。`UnwrapError.failure: Error?` 在 `Result` 取值失败时保留具体错误和原因链，在 `Option` 取值失败时为 `None`。
