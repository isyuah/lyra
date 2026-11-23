fn trace(target: String, argument: String): String {
    "print(" + macroQuote("trace: " + target) + ")"
}
