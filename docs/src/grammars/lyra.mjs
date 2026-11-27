// Minimal TextMate grammar for Lyra code blocks on the documentation site.
// It intentionally covers only lexical categories; the compiler remains the
// source of truth for syntax and semantics.
export default {
  name: "lyra",
  scopeName: "source.lyra",
  patterns: [
    { include: "#comments" },
    { include: "#strings" },
    { include: "#numbers" },
    { include: "#keywords" },
    { include: "#attributes" },
    { include: "#types" },
    { include: "#functions" },
  ],
  repository: {
    comments: {
      patterns: [
        { match: "//.*$", name: "comment.line.double-slash.lyra" },
        { begin: "/\\*", end: "\\*/", name: "comment.block.lyra" },
      ],
    },
    strings: {
      patterns: [
        {
          begin: "br#+\"",
          end: "\"#+",
          name: "string.quoted.raw.bytes.lyra",
        },
        {
          begin: "r#+\"",
          end: "\"#+",
          name: "string.quoted.raw.lyra",
        },
        {
          begin: "b\"",
          end: "\"",
          name: "string.quoted.bytes.lyra",
          patterns: [
            { match: "\\\\.", name: "constant.character.escape.lyra" },
          ],
        },
        {
          begin: "\"",
          end: "\"",
          name: "string.quoted.double.lyra",
          patterns: [
            {
              match: "\\$\\{[^}]*\\}",
              name: "meta.interpolation.lyra",
            },
            { match: "\\\\.", name: "constant.character.escape.lyra" },
          ],
        },
        {
          match: "'(?:\\\\.|[^'\\\\])'",
          name: "string.quoted.single.lyra",
        },
      ],
    },
    numbers: {
      patterns: [
        {
          match:
            "\\b(?:0[xX][0-9a-fA-F_]+|0[bB][01_]+|0[oO][0-7_]+|\\d[\\d_]*\\.?[\\d_]*(?:[eE][+-]?\\d+)?(?:f32|f64)?)\\b",
          name: "constant.numeric.lyra",
        },
      ],
    },
    keywords: {
      patterns: [
        {
          match:
            "\\b(?:fn|val|var|struct|class|enum|trait|type|pub|import|as|if|else|while|for|in|match|return|break|continue|defer|throw|try|catch|async|await|spawn|parallel|taskGroup|blocking|select|const|static|init|self|this|true|false|null|None|Some|Ok|Err|Unit|Bool|Int|UInt|Int8|Int16|Int32|Int64|UInt8|UInt16|UInt32|UInt64|Float|Float32|Float64|String|Char|Bytes|Any|Task|Channel|Option|Result|List|HashMap|HashSet|FrozenList|dyn|implicit|extension|macro|decorator|extern|unsafe|exception|throws)\\b",
          name: "keyword.control.lyra",
        },
      ],
    },
    attributes: {
      patterns: [
        {
          match: "#[A-Za-z_][A-Za-z0-9_]*|@[A-Za-z_][A-Za-z0-9_]*",
          name: "entity.name.function.decorator.lyra",
        },
      ],
    },
    types: {
      patterns: [
        {
          match: "\\b[A-Z][A-Za-z0-9_]*\\b",
          name: "entity.name.type.lyra",
        },
      ],
    },
    functions: {
      patterns: [
        {
          match: "(?<=\\bfn\\s+)[A-Za-z_][A-Za-z0-9_]*",
          name: "entity.name.function.lyra",
        },
      ],
    },
  },
};
