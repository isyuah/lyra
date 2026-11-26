import std.collections.{HashMap, HashSet, stringMap, stringSet}
import std.io.{IoError}
import std.os.{args}
import std.text.{StringOps}

pub enum CliErrorKind {
    ReadArgs(IoError), UnknownOption(String), MissingValue(String), UnexpectedValue(String),
    DuplicateOption(String), MissingRequired(String), UnknownCommand(String),
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
            UnknownCommand(name) => "unknown command ${name}",
        }
    }
}

struct OptionSpec(val name: String, val short: String?, val required: Bool, val flag: Bool)

pub class ParsedCommand(val values: HashMap<String,String>, val flags: HashSet<String>, pub val positionals: List<String>,
    pub val name: String, pub val subcommand: ParsedCommand?) {
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

fn letter(code: UInt32): Bool { (0x61 <= code && code <= 0x7A) || (0x41 <= code && code <= 0x5A) }

fn optionToken(value: String): Bool {
    value.startsWith("--") || (value.startsWith("-") && value.size > 1 && letter(value[1].codePoint))
}

class ArgumentScan(val arguments: List<String>, var index: Int) {
    val values: HashMap<String,String> = stringMap<String>()
    val flags: HashSet<String> = stringSet()
    var wantsHelp: Bool = false

    fn record(spec: OptionSpec, spelling: String, given: String?): Result<Unit, CliError> {
        if values.contains(spec.name) || flags.contains(spec.name) { return Err(CliError(CliErrorKind.DuplicateOption(spec.name))) }
        if spec.flag {
            if given != None { return Err(CliError(CliErrorKind.UnexpectedValue(spelling))) }
            flags.add(spec.name);
        } else {
            val value = match given {
                Some(attached) => attached,
                None => {
                    if index == arguments.size || optionToken(arguments[index]) { return Err(CliError(CliErrorKind.MissingValue(spelling))) }
                    val next = arguments[index]
                    index += 1
                    next
                },
            }
            values.put(spec.name, value);
        }
        Ok({})
    }

    fn helpFlag(spelling: String, given: String?): Result<Unit, CliError> {
        if given != None { return Err(CliError(CliErrorKind.UnexpectedValue(spelling))) }
        if wantsHelp { return Err(CliError(CliErrorKind.DuplicateOption("--help"))) }
        wantsHelp = true
        Ok({})
    }
}

pub class Command(pub val name: String) {
    val specifications: List<OptionSpec> = []
    val commands: List<Command> = []

    fn find(optionName: String): Option<OptionSpec> {
        for spec in specifications {
            if spec.name == optionName || spec.short == Some(optionName) { return Some(spec) }
        }
        None
    }

    fn register(optionName: String, shortName: String?, required: Bool, flag: Bool): Unit {
        if !validOptionName(optionName) || optionName == "--help" { throw Exception("invalid or reserved long option name: ${optionName}") }
        if find(optionName) != None { throw Exception("option already registered: ${optionName}") }
        if let Some(alias) = shortName {
            if alias.size != 2 || !alias.startsWith("-") || !letter(alias[1].codePoint) || alias == "-h" {
                throw Exception("invalid or reserved short option name: ${alias}")
            }
            if find(alias) != None { throw Exception("option already registered: ${alias}") }
        }
        specifications.add(OptionSpec(optionName, shortName, required, flag))
    }

    pub fn option(name: String, required: Bool = false, short: String? = None): Command {
        register(name, short, required, false)
        this
    }

    pub fn flag(name: String, short: String? = None): Command {
        register(name, short, false, true)
        this
    }

    fn findCommand(name: String): Command? {
        for command in commands { if command.name == name { return Some(command) } }
        None
    }

    pub fn subcommand(command: Command): Command {
        if !validOptionName("--" + command.name) { throw Exception("invalid command name: ${command.name}") }
        if findCommand(command.name) != None { throw Exception("command already registered: ${command.name}") }
        commands.add(command)
        this
    }

    pub fn help(): String { usage(name) }

    fn usage(path: String): String {
        val command = if commands.size == 0 { "" } else { " [command]" }
        var text = "Usage: ${path} [options]${command} [--] [args...]\n\nOptions:\n"
        for spec in specifications {
            val value = if spec.flag { "" } else { " <value>" }
            val required = if spec.required { " (required)" } else { "" }
            val alias = match spec.short { Some(shortName) => "${shortName}, ", None => "" }
            text += "  ${alias}${spec.name}${value}${required}\n"
        }
        text += "  -h, --help\n"
        if commands.size != 0 {
            text += "\nCommands:\n"
            for child in commands { text += "  ${child.name}\n" }
        }
        text
    }

    fn longOption(argument: String, state: ArgumentScan): Result<Unit, CliError> {
        val separator = valueSeparator(argument)
        val hasValue = separator < argument.size
        val spelling = if hasValue { argument.substring(0, separator) } else { argument }
        val value: String? = if hasValue { Some(argument.substring(separator + 1)) } else { None }
        if spelling == "--help" { return state.helpFlag(spelling, value) }
        match find(spelling) {
            None => Err(CliError(CliErrorKind.UnknownOption(spelling))),
            Some(spec) => state.record(spec, spelling, value),
        }
    }

    fn shortOptions(argument: String, state: ArgumentScan): Result<Unit, CliError> {
        val size = argument.size
        var offset = 1
        for point in argument.substring(1) {
            val spelling = "-${point}"
            offset += 1
            val hasEquals = offset < size && argument[offset].codePoint == 0x3D
            if spelling == "-h" {
                state.helpFlag(spelling, if hasEquals { Some("") } else { None })?
                continue
            }
            match find(spelling) {
                None => return Err(CliError(CliErrorKind.UnknownOption(spelling))),
                Some(spec) => {
                    if !spec.flag {
                        val value: String? = if offset < size { Some(argument.substring(if hasEquals { offset + 1 } else { offset })) } else { None }
                        return state.record(spec, spelling, value)
                    }
                    state.record(spec, spelling, if hasEquals { Some("") } else { None })?
                },
            }
        }
        Ok({})
    }

    fn scan(arguments: List<String>, start: Int, path: String): Result<CliResult, CliError> {
        val state = ArgumentScan(arguments, start)
        val positionals: List<String> = []
        var optionsEnabled = true
        var selected: ParsedCommand? = None
        while state.index < arguments.size {
            val argument = arguments[state.index]
            state.index += 1
            if !optionsEnabled || argument == "-" || !argument.startsWith("-") {
                if optionsEnabled && commands.size != 0 {
                    if state.wantsHelp { return Ok(CliResult.Help(usage(path))) }
                    match findCommand(argument) {
                        None => return Err(CliError(CliErrorKind.UnknownCommand(argument))),
                        Some(command) => match command.scan(arguments, state.index, "${path} ${argument}")? {
                            Help(text) => return Ok(CliResult.Help(text)),
                            Run(parsed) => { selected = Some(parsed) },
                        },
                    }
                    break
                }
                positionals.add(argument)
                continue
            }
            if argument == "--" {
                optionsEnabled = false
                continue
            }
            if argument.startsWith("--") { longOption(argument, state)? }
            else { shortOptions(argument, state)? }
        }
        if state.wantsHelp { return Ok(CliResult.Help(usage(path))) }
        for spec in specifications {
            if spec.required && !state.values.contains(spec.name) { return Err(CliError(CliErrorKind.MissingRequired(spec.name))) }
        }
        Ok(CliResult.Run(ParsedCommand(state.values, state.flags, positionals, name, selected)))
    }
}

pub fn parseArgs(spec: Command, arguments: List<String>): Result<CliResult, CliError> { spec.scan(arguments, 0, spec.name) }

pub fn parseArgs(spec: Command): Result<CliResult, CliError> {
    match args() {
        Ok(arguments) => parseArgs(spec, arguments),
        Err(problem) => Err(CliError(CliErrorKind.ReadArgs(problem))),
    }
}
