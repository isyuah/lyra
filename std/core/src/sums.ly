/// `Option<T>` 的映射、回退和转换方法，无需导入。
pub extension OptionOps<T> for Option<T> {
    /// 对 `Some` 的值应用 `transform`；`None` 原样返回。
    fn map<R>(transform: (T) -> R): Option<R> { Some(transform(this?)) }
    /// 对 `Some` 的值应用返回 `Option` 的函数，不再额外包一层。
    fn flatMap<R>(transform: (T) -> Option<R>): Option<R> { transform(this?) }
    /// 保留 `Some`；只在遇到 `None` 时调用 `fallback`。
    ///
    /// ```lyra
    /// val value: Int? = None
    /// value.orElse(() => Some(42))
    /// ```
    fn orElse(fallback: () -> Option<T>): Option<T> {
        match this { Some(_) => this, None => fallback() }
    }
    /// 将 `Some(value)` 转成 `Ok(value)`；将 `None` 转成 `Err(error())`。
    ///
    /// ```lyra
    /// val port: Int? = None
    /// val configured = port.toResult(() => "missing port")
    /// ```
    fn toResult<E>(error: () -> E): Result<T,E> {
        match this { Some(value) => Ok(value), None => Err(error()) }
    }
}

/// `Result<T,E>` 的映射、错误恢复和转换方法，无需导入。
pub extension ResultOps<T,E> for Result<T,E> {
    /// 映射 `Ok` 的值，保留 `Err`。
    fn map<R>(transform: (T) -> R): Result<R,E> { Ok(transform(this?)) }
    /// 将 `Ok` 的值交给另一个返回 `Result` 的函数，保留 `Err`。
    fn flatMap<R>(transform: (T) -> Result<R,E>): Result<R,E> { transform(this?) }
    /// 转换 `Err` 的错误值，保留 `Ok`。
    fn mapErr<F>(transform: (E) -> F): Result<T,F> {
        match this { Ok(value) => Ok(value), Err(error) => this.remapFailure<F>(transform(error)) }
    }
    /// 只在失败时调用 `fallback`，并将原来的错误值传给它。
    fn orElse<F>(fallback: (E) -> Result<T,F>): Result<T,F> {
        match this { Ok(value) => Ok(value), Err(error) => fallback(error) }
    }
    /// 成功时返回结果值，失败时返回已求值的 `defaultValue`。
    fn unwrapOr(defaultValue: T): T {
        match this { Ok(value) => value, Err(_) => defaultValue }
    }
    /// 成功时返回结果值，只在失败时计算回退值。
    fn unwrapOrElse(fallback: (E) -> T): T {
        match this { Ok(value) => value, Err(error) => fallback(error) }
    }
    /// 将 `Ok(value)` 转成 `Some(value)`；丢弃错误信息并将 `Err` 转成 `None`。
    fn toOption(): Option<T> {
        match this { Ok(value) => Some(value), Err(_) => None }
    }
}

/// 调换外层 `Option` 与内层 `Result`。
pub extension OptionalResultOps<T,E> for Option<Result<T,E>> {
    /// 将 `None` 变为 `Ok(None)`、`Some(Ok(value))` 变为 `Ok(Some(value))`，保留失败。
    fn transpose(): Result<Option<T>,E> {
        match this { Some(result) => Ok(Some(result?)), None => Ok(None) }
    }
}

/// 调换外层 `Result` 与内层 `Option`。
pub extension ResultOptionalOps<T,E> for Result<Option<T>,E> {
    /// 将 `Ok(None)` 变为 `None`、`Ok(Some(value))` 变为 `Some(Ok(value))`，把失败包成 `Some(Err(...))`。
    fn transpose(): Option<Result<T,E>> {
        match this {
            Ok(Some(value)) => Some(Ok(value)),
            Ok(None) => None,
            Err(_) => Some(this.carryFailure<T>()),
        }
    }
}
