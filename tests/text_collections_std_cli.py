from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from platform_cli import executable


COMPILER = Path(sys.argv.pop(1)).resolve()


class TextAndCollections(unittest.TestCase):
    def run_program(self, source, expected):
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory)
            sources = package / 'src'
            sources.mkdir()
            (package / 'lyra.json5').write_text('{name:"test/text-collections",version:"0.1.0"}', encoding='utf-8')
            (sources / 'main.ly').write_text(source, encoding='utf-8')
            for profile, flags in (('debug', []), ('release', ['--release'])):
                with self.subTest(profile=profile):
                    app = executable(package / profile)
                    build = subprocess.run([COMPILER, 'build', package, *flags, '-o', app], capture_output=True, timeout=30)
                    self.assertEqual(build.returncode, 0, build.stderr.decode())
                    result = subprocess.run([app], capture_output=True, timeout=10)
                    self.assertEqual((result.returncode, result.stdout.decode(), result.stderr), (0, expected, b''))

    def test_unicode_slices_prefixes_whitespace_and_literal_split(self):
        self.run_program(r'''
import std.text.{StringOps}

fn main(): Unit {
    val text = "中🙂e\u{0301}\0"
    print(text.substring(1, 4))
    print(text.substring(3))
    print(text.slice(0..1))
    print(text.slice(5..<5) == "")
    print(text.startsWith("中🙂") && text.startsWith("") && !text.startsWith("中🙂e\u{0301}\0x"))
    print(text.endsWith("\0") && text.endsWith("") && !text.endsWith("中🙂"))
    print("\u{2003}\u{00A0}\t中\u{2002}🙂\r\n\u{3000}".trim())
    print("\u{2028}\u{202F}".trim() == "")
    print("\u{200B}".trim() == "\u{200B}")
    for part in "🙂甲🙂🙂乙🙂".split("🙂") { print("[${part}]") }
    for part in "abababa".split("aba") { print("[${part}]") }
    for part in "左\0右".split("\0") { print(part) }
    print("".split("+").size)
    print(try { "🙂".substring(0, 2); false } catch { true })
    print(try { "🙂".slice(1..0); false } catch { true })
    print(try { "🙂".split(""); false } catch { true })
}
''', '🙂e\u0301\n\u0301\0\n中🙂\ntrue\ntrue\ntrue\n中\u2002🙂\ntrue\ntrue\n[]\n[甲]\n[]\n[乙]\n[]\n[]\n[b]\n[]\n左\n右\n1\ntrue\ntrue\ntrue\n')

    def test_parse_int_checks_entire_input_and_both_integer_limits(self):
        self.run_program(r'''
import std.text.{ParseIntError}
import std.text.{StringOps}

fn format(result: Result<Int, ParseIntError>): String {
    match result {
        Ok(value) => value.toString(),
        Err(ParseIntError.Empty) => "empty",
        Err(ParseIntError.InvalidRadix) => "radix",
        Err(ParseIntError.InvalidDigit(index)) => "digit ${index}",
        Err(ParseIntError.Overflow) => "overflow",
    }
}

fn main(): Unit {
    print(format("+7F".parseInt(radix: 16)))
    print(format("z".parseInt(36)))
    print(format("-0".parseInt()))
    print(format("9223372036854775807".parseInt()))
    print(format("-8000000000000000".parseInt(16)))
    print(format("9223372036854775808".parseInt()))
    print(format("-9223372036854775809".parseInt()))
    print(format("9223372036854775808000000x".parseInt()))
    print(format("12 3".parseInt()))
    print(format("0xff".parseInt(16)))
    print(format("11\0".parseInt()))
    print(format("1🙂".parseInt()))
    print(format("2".parseInt(2)))
    print(format("-".parseInt()))
    print(format("".parseInt()))
    print(format("10".parseInt(37)))
}
''', '127\n35\n0\n9223372036854775807\n-9223372036854775808\noverflow\noverflow\ndigit 25\ndigit 2\ndigit 1\ndigit 2\ndigit 1\ndigit 0\nempty\nempty\nradix\n')

    def test_contains_and_replace_use_literal_non_overlapping_matches(self):
        self.run_program(r'''
import std.text.{StringOps}

fn main(): Unit {
    #assert("ababcababd".contains("ababd") && !"ababcababd".contains("ababe"))
    #assert("中🙂\0".contains("🙂\0") && "".contains("") && !"".contains("中"))
    #assert("ababa".replace("aba", "X") == "Xba")
    #assert("🙂\0🙂\0".replace("🙂\0", "中") == "中中")
    #assert("中🙂".replace("", "+") == "+中+🙂+")
    #assert("".replace("", "-") == "-")
    #assert("unchanged".replace("missing", "!") == "unchanged")
    #assert("aaaa".replace("a", "aa") == "aaaaaaaa")
    #assert("data-data".replace("data", "") == "-")
}
''', '')

    def test_index_of_parsing_floats_and_raw_byte_operations(self):
        self.run_program(r'''
import std.text.{ParseFloatError, StringOps}

const combined: Bytes = (b"\x00" + b"\xff!").slice(0..1)

fn main(): Unit {
    #assert("中🙂e\0".indexOf("🙂e") == Some(1))
    #assert("中🙂e\0".indexOf("\0") == Some(3))
    #assert("ababcababd".indexOf("ababd") == Some(5))
    #assert("🙂".indexOf("中").isNone() && "".indexOf("") == Some(0))
    #assert("+1.25e2".parseFloat().unwrap() == 125.0)
    #assert("-.5".parseFloat().unwrap() == -0.5)
    #assert("".parseFloat() == Err(ParseFloatError.Empty))
    #assert("1e309".parseFloat() == Err(ParseFloatError.OutOfRange))
    #assert("1e-4000".parseFloat() == Err(ParseFloatError.OutOfRange))
    for text in [" 2.5", "2.5 ", "0x10", "+-2", "2e", "NaN", "inf", "1\0", "1,5"] {
        #assert(text.parseFloat() == Err(ParseFloatError.Invalid))
    }
    var bytes = combined
    bytes += b"\x80"
    #assert(bytes.toHex() == "00ff80")
    #assert(Bytes.fromHex("00FF80").unwrap() == bytes)
    #assert(Bytes.fromHex("").unwrap() == b"")
    #assert(Bytes.fromHex("0").isNone() && Bytes.fromHex("x0").isNone() && Bytes.fromHex("🙂").isNone())
    #assert(bytes.slice(1..<3) == b"\xff\x80" && bytes.slice(3..<3) == b"")
    #assert(try { bytes.slice(0..3); false } catch problem: BoundsError { true })
}
''', '')

    def test_frozen_lists_share_read_only_operations_and_keep_optional_slots(self):
        self.run_program('''
import std.collections.{FrozenListOps, ListOps, map}

const first: Int = FrozenList([9]).firstOrNone().unwrap()

fn main(): Unit {
    #assert(first == 9)
    val input: List<Int?> = [None, Some(4), Some(2), None]
    val frozen = FrozenList(input)
    #assert(input.firstOrNone() == Some(None) && input.lastOrNone() == Some(None))
    #assert(frozen.firstOrNone() == Some(None) && frozen.lastOrNone() == Some(None))
    #assert(frozen.getOrNone(1) == Some(Some(4)))
    #assert(input.getOrNone(-1).isNone() && frozen.getOrNone(4).isNone())
    val empty: List<Int> = []
    #assert(empty.firstOrNone().isNone() && FrozenList(empty).lastOrNone().isNone())
    input[1] = Some(7)
    val slice = frozen.slice(1..2)
    #assert(slice.size == 2 && slice[0] == Some(4))
    val doubled = slice.map { (it ?? 0) * 2 }.filter { it > 4 }
    #assert(doubled.size == 1 && doubled[0] == 8)
    #assert(map(frozen) { it ?? 0 }[1] == 4)
    #assert(frozen.filter { it != None }.size == 2)
    #assert(frozen.fold(0) { total, value => total + (value ?? 0) } == 6)
    #assert(frozen.any { it == Some(2) } && frozen.find { it == None } == Some(None))
    #assert(frozen.sortedBy { it ?? 0 }[2] == Some(2))
}
''', '')

    def test_stable_sorted_by_sum_distinct_and_independent_shallow_keys(self):
        self.run_program('''
import std.collections.{ListOps}
import std.collections.{distinct}
import std.collections.{hashMap}
import std.collections.{sum}

class Item(val rank: Int, var label: String)

fn main(): Unit {
    val input = [Item(2, "a"), Item(1, "b"), Item(2, "c"), Item(0, "d"), Item(1, "e")]
    var visited = ""
    val sorted = input.sortedBy { visited += it.label; it.rank }
    print(visited)
    for item in sorted { print(item.label) }
    print(input[0].label)
    sorted[0].label = "shared"
    print(input[3].label)
    sorted[0] = Item(10, "replacement")
    print(input[3].label)
    print(sum(input.map { it.rank }))
    val empty: List<Int> = []
    print(sum(empty))
    print(empty.sortedBy { it }.size)
    for item in distinct([3, 1, 3, 2, 1]) { print(item) }

    val map = hashMap<Item, Int>((_) => { 0 }, (left, right) => { left == right })
    val entry = Item(1, "original")
    map.put(entry, 7)
    val keys = map.keys()
    map.remove(entry)
    map.put(Item(2, "new"), 9)
    entry.label = "updated"
    print(keys.size)
    print(keys[0] == entry)
    print(keys[0].label)
    print(map.keys()[0].label)
}
''', 'abcde\nd\nb\ne\na\nc\na\nshared\nshared\n6\n0\n0\n3\n1\n2\n1\ntrue\nupdated\nnew\n')


if __name__ == '__main__':
    unittest.main()
