pub fn typeName(value: Any): String { value.typeName() }
pub fn fieldCount(value: Any): Int { value.fieldCount() }
pub fn fieldNames(value: Any): List<String> { value.fieldNames() }
pub fn field(value: Any, name: String): Option<Any> { value.field(name) }
