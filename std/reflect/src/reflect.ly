/// 返回原值的可读运行时类型名，而不是装箱后的 `Any`。
pub fn typeName(value: Any): String { value.typeName() }
/// 统计结构体或类的公开字段；不可反射字段的值返回 `0`。
pub fn fieldCount(value: Any): Int { value.fieldCount() }
/// 列出结构体或类的公开字段名，不包含私有字段或计算属性。
pub fn fieldNames(value: Any): List<String> { value.fieldNames() }
/// 按名称读取公开字段，保留值的运行时类型；字段不存在或不公开时返回 `None`。
pub fn field(value: Any, name: String): Option<Any> { value.field(name) }
