pub extension OptionOps<T> for Option<T> {
    fn map<R>(transform: (T) -> R): Option<R> { Some(transform(this?)) }
    fn flatMap<R>(transform: (T) -> Option<R>): Option<R> { transform(this?) }
    fn orElse(fallback: () -> Option<T>): Option<T> {
        match this { Some(_) => this, None => fallback() }
    }
    fn toResult<E>(error: () -> E): Result<T,E> {
        match this { Some(value) => Ok(value), None => Err(error()) }
    }
}

pub extension ResultOps<T,E> for Result<T,E> {
    fn map<R>(transform: (T) -> R): Result<R,E> { Ok(transform(this?)) }
    fn flatMap<R>(transform: (T) -> Result<R,E>): Result<R,E> { transform(this?) }
    fn mapErr<F>(transform: (E) -> F): Result<T,F> {
        match this { Ok(value) => Ok(value), Err(error) => this.remapFailure<F>(transform(error)) }
    }
    fn orElse<F>(fallback: (E) -> Result<T,F>): Result<T,F> {
        match this { Ok(value) => Ok(value), Err(error) => fallback(error) }
    }
    fn unwrapOr(defaultValue: T): T {
        match this { Ok(value) => value, Err(_) => defaultValue }
    }
    fn unwrapOrElse(fallback: (E) -> T): T {
        match this { Ok(value) => value, Err(error) => fallback(error) }
    }
    fn toOption(): Option<T> {
        match this { Ok(value) => Some(value), Err(_) => None }
    }
}

pub extension OptionalResultOps<T,E> for Option<Result<T,E>> {
    fn transpose(): Result<Option<T>,E> {
        match this { Some(result) => Ok(Some(result?)), None => Ok(None) }
    }
}

pub extension ResultOptionalOps<T,E> for Result<Option<T>,E> {
    fn transpose(): Option<Result<T,E>> {
        match this {
            Ok(Some(value)) => Some(Ok(value)),
            Ok(None) => None,
            Err(_) => Some(this.carryFailure<T>()),
        }
    }
}
