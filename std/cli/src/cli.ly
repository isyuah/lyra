import std.collections.{HashMap, HashSet, stringMap, stringSet}
import std.io.{IoError}
import std.os.{args}
import std.text.{StringOps}

pub enum CliErrorKind {
    ReadArgs(IoError), UnknownOption(String), MissingValue(String), UnexpectedValue(String),
    DuplicateOption(String), MissingRequired(String),
}

pub struct CliError(pub val kind: CliErrorKind) {
    pub fn message(): String {
        match kind {
            ReadArgs(problem) => problem.message(),
            UnknownOption(name) => "unknown option ${name}",
            MissingValue(name) => "${name} requires a value",
            UnexpectedValue(name) => "${name} does not take a value",
            DuplicateOption(name) => "${name} was supplied more than once",
            MissingRequired(name) => "missing required option ${name}",
        }
    }
}

struct OptionSpec(val name: String, val required: Bool, val flag: Bool)

pub class ParsedCommand(val values: HashMap<String,String>, val flags: HashSet<String>, pub val positionals: List<String>) {
    pub fn value(name: String): String? { values.get(name) }
    pub fn flag(name: String): Bool { flags.contains(name) }
}

pub enum CliResult { Run(ParsedCommand), Help(String) }

fn validOptionName(name: String): Bool {
    if name.size <= 2 || !name.startsWith("--") { return false }
    var allowHyphen = false
    for point in name.substring(2) {
        val code = point.codePoint
        val alphaNumeric = (0x61 <= code && code <= 0x7A) || (0x41 <= code && code <= 0x5A) || (0x30 <= code && code <= 0x39)
        if !(alphaNumeric || (allowHyphen && code == 0x2D)) { return false }
        allowHyphen = true
    }
    true
}

fn valueSeparator(argument: String): Int {
    var position = 0
    for point in argument {
        if point.codePoint == 0x3D { break }
        position += 1
    }
    position
}

pub class Command(pub val name: String) {
    val specifications: List<OptionSpec> = []

    fn find(optionName: String): Option<OptionSpec> {
        for spec in specifications {
            if spec.name == optionName { return Some(spec) }
        }
        None
    }

    fn register(optionName: String, required: Bool, flag: Bool): Unit {
        if !validOptionName(optionName) || optionName == "--help" { throw Exception("invalid or reserved long option name: ${optionName}") }
        if find(optionName) != None { throw Exception("option already registered: ${optionName}") }
        specifications.add(OptionSpec(optionName, required, flag))
    }

    pub fn option(name: String, required: Bool = false): Command {
        register(name, required, false)
        this
    }

    pub fn flag(name: String): Command {
        register(name, false, true)
        this
    }

    pub fn help(): String {
        var text = "Usage: ${name} [options] [--] [args...]\n\nOptions:\n"
        for spec in specifications {
            val value = if spec.flag { "" } else { " <value>" }
            val required = if spec.required { " (required)" } else { "" }
            text += "  ${spec.name}${value}${required}\n"
        }
        text + "  --help\n"
    }

    fn scan(arguments: List<String>): Result<CliResult, CliError> {
        val values = stringMap<String>()
        val flags = stringSet()
        val positionals: List<String> = []
        var optionsEnabled = true
        var wantsHelp = false
        var index = 0
        while index < arguments.size {
            val argument = arguments[index]
            index += 1
            if !optionsEnabled || argument == "-" || !argument.startsWith("-") {
                positionals.add(argument)
                continue
            }
            if argument == "--" {
                optionsEnabled = false
                continue
            }
            val separator = valueSeparator(argument)
            val hasValue = separator < argument.size
            val optionName = if hasValue { argument.substring(0, separator) } else { argument }
            if optionName == "--help" {
                if hasValue { return Err(CliError(CliErrorKind.UnexpectedValue(optionName))) }
                if wantsHelp { return Err(CliError(CliErrorKind.DuplicateOption(optionName))) }
                wantsHelp = true
                continue
            }
            match find(optionName) {
                None => return Err(CliError(CliErrorKind.UnknownOption(optionName))),
                Some(spec) => {
                    if values.contains(optionName) || flags.contains(optionName) {
                        return Err(CliError(CliErrorKind.DuplicateOption(optionName)))
                    }
                    if spec.flag {
                        if hasValue { return Err(CliError(CliErrorKind.UnexpectedValue(optionName))) }
                        flags.add(optionName);
                    } else if hasValue {
                        values.put(optionName, argument.substring(separator + 1));
                    } else {
                        if index == arguments.size || arguments[index].startsWith("--") {
                            return Err(CliError(CliErrorKind.MissingValue(optionName)))
                        }
                        values.put(optionName, arguments[index]);
                        index += 1
                    }
                },
            }
        }
        if wantsHelp { return Ok(CliResult.Help(help())) }
        for spec in specifications {
            if spec.required && !values.contains(spec.name) { return Err(CliError(CliErrorKind.MissingRequired(spec.name))) }
        }
        Ok(CliResult.Run(ParsedCommand(values, flags, positionals)))
    }
}

pub fn parseArgs(spec: Command, arguments: List<String>): Result<CliResult, CliError> { spec.scan(arguments) }

pub fn parseArgs(spec: Command): Result<CliResult, CliError> {
    match args() {
        Ok(arguments) => parseArgs(spec, arguments),
        Err(problem) => Err(CliError(CliErrorKind.ReadArgs(problem))),
    }
}
