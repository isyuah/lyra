import std.cli.{ParsedCommand}
import std.io.{IoError, readFile}
import std.os.{env}
import std.text.{StringOps}
import std.toml.{TomlCodec, TomlValue, parseToml, tomlCodecBool, tomlCodecInt, tomlCodecString}

/// 加载数据或解析某个配置键时的失败分类。
pub enum ConfigErrorKind {
    /// 读取配置文件失败，保留底层 I/O 错误。
    ReadFile(IoError),
    /// 输入不是可解析的 TOML 文档。
    InvalidToml,
    /// 查询环境变量失败，保留底层 I/O 错误。
    ReadEnv(IoError),
    /// 点分键含有空段，例如 `server..port`。
    InvalidKey,
    /// 选中的值无法解码，或点分键路径经过一个非表节点。
    InvalidValue,
}

/// 配置错误与出错来源，错误消息不回显原始配置值。
pub struct ConfigError(
    /// 用于匹配失败原因的分类。
    pub val kind: ConfigErrorKind,
    /// 文件、TOML 键路径、环境变量或命令行选项等来源标识。
    pub val source: String
) {
    /// 格式化来源与失败原因，适用于日志或终端提示。
    pub fn message(): String {
        match kind {
            ReadFile(problem) => "${source}: ${problem.message()}",
            InvalidToml => "${source}: invalid TOML document",
            ReadEnv(problem) => "${source}: ${problem.message()}",
            InvalidKey => "${source}: invalid dotted configuration key",
            InvalidValue => "${source}: invalid configuration value",
        }
    }
}

/// 描述一个类型化配置项，包括点分路径、解码规则、默认值与可选的 CLI/环境变量绑定。
pub struct ConfigKey<T>(
    /// TOML 表中的点分路径，如 `server.port`。
    pub val path: String, val codec: TomlCodec<T>, val parseText: (String) -> Result<T,String>,
                        val defaultValue: T, val cliName: String? = None, val envName: String? = None) {
    fn fromText(text: String, source: String): Result<T,ConfigError> {
        match parseText(text) {
            Ok(value) => Ok(value),
            Err(_) => Err(ConfigError(ConfigErrorKind.InvalidValue, source)),
        }
    }

    fn fromToml(node: TomlValue, source: String): Result<T,ConfigError> {
        match codec.read(node, path) {
            Ok(value) => Ok(value),
            Err(_) => Err(ConfigError(ConfigErrorKind.InvalidValue, source)),
        }
    }
}

/// 声明自定义类型的配置键；`codec` 处理 TOML，`parseText` 处理 CLI 或环境变量文本。
pub fn configKey<T>(path: String, codec: TomlCodec<T>, parseText: (String) -> Result<T,String>,
                    defaultValue: T, cliName: String? = None, envName: String? = None): ConfigKey<T> {
    ConfigKey<T>(path, codec, parseText, defaultValue, cliName, envName)
}

/// 声明字符串键；CLI 和环境变量的文本原样使用，空串也是已设置的值。
pub fn stringKey(path: String, defaultValue: String, cliName: String? = None, envName: String? = None): ConfigKey<String> {
    configKey(path, tomlCodecString(), (text) => { Ok(text) }, defaultValue, cliName, envName)
}

/// 声明整数键；CLI 和环境变量按完整十进制 `Int` 解析。
pub fn intKey(path: String, defaultValue: Int, cliName: String? = None, envName: String? = None): ConfigKey<Int> {
    configKey(path, tomlCodecInt(), (text) => {
        match text.parseInt() { Ok(value) => Ok(value), Err(_) => Err("expected an Int") }
    }, defaultValue, cliName, envName)
}

/// 声明布尔键；文本只接受 `true` 或 `false`，出现的 CLI 开关算作 `true`。
pub fn boolKey(path: String, defaultValue: Bool, cliName: String? = None, envName: String? = None): ConfigKey<Bool> {
    configKey(path, tomlCodecBool(), (text) => {
        match text { "true" => Ok(true), "false" => Ok(false), _ => Err("expected a Bool") }
    }, defaultValue, cliName, envName)
}

fn configText(text: String, source: String): Result<Config, ConfigError> {
    match parseToml(text) {
        Ok(document) => Ok(Config(Some(document), source)),
        Err(_) => Err(ConfigError(ConfigErrorKind.InvalidToml, source)),
    }
}

/// 保存可选的 TOML 文档，按优先级解析类型化配置项。
pub class Config(val document: TomlValue?, val source: String) {
    /// 创建不含 TOML 文档的配置；仍可读取 CLI、环境变量和默认值。
    pub static fn empty(): Config { Config(None, "TOML") }

    /// 从 UTF-8 文件加载 TOML；区分文件读取错误和 TOML 格式错误。
    pub static fn load(path: String): Result<Config, ConfigError> {
        match readFile(path) {
            Ok(text) => configText(text, path),
            Err(problem) => Err(ConfigError(ConfigErrorKind.ReadFile(problem), path)),
        }
    }

    /// 从内存中的 TOML 文本加载，不读取文件。
    pub static fn fromToml(text: String): Result<Config, ConfigError> { configText(text, "TOML") }

    /// 依次查 CLI、环境变量、TOML 和默认值；仅未设置时才回退，已设置但非法的值直接报错。
    pub fn get<T>(key: ConfigKey<T>, arguments: ParsedCommand? = None): Result<T,ConfigError> {
        val parts = key.path.split(".")
        for part in parts {
            if part == "" { return Err(ConfigError(ConfigErrorKind.InvalidKey, key.path)) }
        }
        if let Some(command) = arguments {
            if let Some(name) = key.cliName {
                if let Some(value) = command.value(name) { return key.fromText(value, "CLI ${name}") }
                if command.flag(name) { return key.fromText("true", "CLI ${name}") }
            }
        }
        if let Some(name) = key.envName {
            match env(name) {
                Ok(Some(value)) => return key.fromText(value, "env ${name}"),
                Ok(None) => {},
                Err(problem) => return Err(ConfigError(ConfigErrorKind.ReadEnv(problem), "env ${name}")),
            }
        }
        if let Some(root) = document {
            val location = "${source}: ${key.path}"
            var node = root
            for part in parts {
                if !node.isTable() { return Err(ConfigError(ConfigErrorKind.InvalidValue, location)) }
                node = node.field(part)
                if node.missing() { return Ok(key.defaultValue) }
            }
            return key.fromToml(node, location)
        }
        Ok(key.defaultValue)
    }
}
