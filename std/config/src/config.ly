import std.cli.ParsedCommand
import std.io.IoError
import std.io.readFile
import std.os.env
import std.text.StringOps
import std.toml.TomlCodec
import std.toml.TomlValue
import std.toml.parseToml
import std.toml.tomlCodecBool
import std.toml.tomlCodecInt
import std.toml.tomlCodecString

pub enum ConfigErrorKind { ReadFile(IoError), InvalidToml, ReadEnv(IoError), InvalidKey, InvalidValue }

pub struct ConfigError(pub val kind: ConfigErrorKind, pub val source: String) {
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

pub struct ConfigKey<T>(pub val path: String, val codec: TomlCodec<T>, val parseText: (String) -> Result<T,String>,
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

pub fn configKey<T>(path: String, codec: TomlCodec<T>, parseText: (String) -> Result<T,String>,
                    defaultValue: T, cliName: String? = None, envName: String? = None): ConfigKey<T> {
    ConfigKey<T>(path, codec, parseText, defaultValue, cliName, envName)
}

pub fn stringKey(path: String, defaultValue: String, cliName: String? = None, envName: String? = None): ConfigKey<String> {
    configKey(path, tomlCodecString(), (text) => { Ok(text) }, defaultValue, cliName, envName)
}

pub fn intKey(path: String, defaultValue: Int, cliName: String? = None, envName: String? = None): ConfigKey<Int> {
    configKey(path, tomlCodecInt(), (text) => {
        match text.parseInt() { Ok(value) => Ok(value), Err(_) => Err("expected an Int") }
    }, defaultValue, cliName, envName)
}

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

pub class Config(val document: TomlValue?, val source: String) {
    pub static fn empty(): Config { Config(None, "TOML") }

    pub static fn load(path: String): Result<Config, ConfigError> {
        match readFile(path) {
            Ok(text) => configText(text, path),
            Err(problem) => Err(ConfigError(ConfigErrorKind.ReadFile(problem), path)),
        }
    }

    pub static fn fromToml(text: String): Result<Config, ConfigError> { configText(text, "TOML") }

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
