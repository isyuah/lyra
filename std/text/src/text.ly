extern fn lyraTextCopyRange(text: String, start: Int, end: Int): String;

pub enum ParseIntError { Empty, InvalidRadix, InvalidDigit(Int), Overflow }

fn scalarByteWidth(point: UInt32): Int {
    if point < 0x80 { 1 } else if point < 0x800 { 2 } else if point < 0x10000 { 3 } else { 4 }
}

fn unicodeWhitespace(point: UInt32): Bool {
    (0x09 <= point && point <= 0x0D) || point == 0x20 || point == 0x85 || point == 0xA0 || point == 0x1680 ||
        (0x2000 <= point && point <= 0x200A) || point == 0x2028 || point == 0x2029 || point == 0x202F ||
        point == 0x205F || point == 0x3000
}

fn byteMatch(text: Bytes, fragment: Bytes, start: Int): Bool {
    for index in 0..<fragment.size {
        if text[start + index] != fragment[index] { return false }
    }
    true
}

fn prefixLengths(pattern: Bytes): List<Int> {
    val lengths = [0]
    var matched = 0
    for index in 1..<pattern.size {
        val byte = pattern[index]
        while matched > 0 && byte != pattern[matched] { matched = lengths[matched - 1] }
        if byte == pattern[matched] { matched += 1 }
        lengths.add(matched)
    }
    lengths
}

fn asciiDigit(byte: UInt8): Int {
    if 0x30 <= byte && byte <= 0x39 { byte - 0x30 }
    else if 0x41 <= byte && byte <= 0x5A { byte - 0x41 + 10 }
    else if 0x61 <= byte && byte <= 0x7A { byte - 0x61 + 10 }
    else { -1 }
}

pub extension StringOps for String {
    fn startsWith(prefix: String): Bool {
        val text = this.toBytes()
        val pattern = prefix.toBytes()
        pattern.size <= text.size && byteMatch(text, pattern, 0)
    }

    fn endsWith(suffix: String): Bool {
        val text = this.toBytes()
        val pattern = suffix.toBytes()
        val start = text.size - pattern.size
        start >= 0 && byteMatch(text, pattern, start)
    }

    fn substring(start: Int, end: Int): String {
        if start < 0 || end < start { throw Exception("invalid String substring bounds") }
        var startByte = 0
        var endByte = 0
        var index = 0
        for scalar in this {
            if index == start { startByte = endByte }
            if index == end { break }
            endByte += scalarByteWidth(scalar.codePoint)
            index += 1
        }
        if index != end { throw Exception("String substring end is out of bounds") }
        if start == end { return "" }
        lyraTextCopyRange(this, startByte, endByte)
    }

    fn substring(start: Int): String { this.substring(start, this.size) }

    fn slice(range: Range<Int>): String {
        if !range.inclusive { return this.substring(range.start, range.end) }
        if range.end < range.start || range.end == 9223372036854775807 {
            throw Exception("invalid String slice bounds")
        }
        this.substring(range.start, range.end + 1)
    }

    fn split(separator: String): List<String> {
        val pattern = separator.toBytes()
        if pattern.size == 0 { throw Exception("String split separator must not be empty") }
        val lengths = prefixLengths(pattern)
        val text = this.toBytes()
        val output: List<String> = []
        var matched = 0
        var start = 0
        for index in 0..<text.size {
            val byte = text[index]
            while matched > 0 && byte != pattern[matched] { matched = lengths[matched - 1] }
            if byte == pattern[matched] { matched += 1 }
            if matched == pattern.size {
                val next = index + 1
                output.add(lyraTextCopyRange(this, start, next - pattern.size))
                start = next
                matched = 0
            }
        }
        output.add(lyraTextCopyRange(this, start, text.size))
        output
    }

    fn trim(): String {
        var offset = 0
        var start = 0
        var end = 0
        for scalar in this {
            val point = scalar.codePoint
            val next = offset + scalarByteWidth(point)
            if !unicodeWhitespace(point) {
                if end == 0 { start = offset }
                end = next
            }
            offset = next
        }
        lyraTextCopyRange(this, start, end)
    }

    fn parseInt(radix: Int = 10): Result<Int, ParseIntError> {
        if radix < 2 || radix > 36 { return Err(ParseIntError.InvalidRadix) }
        val bytes = this.toBytes()
        if bytes.size == 0 { return Err(ParseIntError.Empty) }
        val negative = bytes[0] == 0x2D
        val start = if negative || bytes[0] == 0x2B { 1 } else { 0 }
        if start == bytes.size { return Err(ParseIntError.Empty) }
        val limit = if negative { -9223372036854775808 } else { -9223372036854775807 }
        val cutoff = limit / radix
        var value = 0
        var overflow = false
        for index in start..<bytes.size {
            val digit = asciiDigit(bytes[index])
            if digit < 0 || digit >= radix { return Err(ParseIntError.InvalidDigit(index)) }
            if !overflow {
                if value < cutoff { overflow = true }
                else {
                    value *= radix
                    if value < limit + digit { overflow = true }
                    else { value -= digit }
                }
            }
        }
        if overflow { return Err(ParseIntError.Overflow) }
        Ok(if negative { value } else { -value })
    }
}
