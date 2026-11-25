import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()

IMPORTS = '''import std.json.jsonArray
import std.json.jsonBool
import std.json.jsonFloat
import std.json.jsonInt
import std.json.jsonNull
import std.json.jsonObject
import std.json.jsonString
import std.json.parseJson
'''


class JsonBuilders(unittest.TestCase):
    def run_source(self, source, *flags):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'lyra.json5').write_text('{name:"app",version:"0.1.0"}', encoding='utf-8')
            (root / 'src').mkdir()
            (root / 'src/main.ly').write_text(IMPORTS + source, encoding='utf-8')
            run = subprocess.run([COMPILER, 'run', root, *flags], capture_output=True, timeout=60,
                                 env={**os.environ, 'LYRA_CACHE': str(root / 'cache')})
        self.assertEqual(run.returncode, 0, run.stderr.decode())
        return run.stdout.decode().splitlines()

    def test_snapshots_preserve_exact_values_and_insertion_order(self):
        source = r'''
fn main(): Unit {
    val rows = jsonArray().add(jsonInt(-9223372036854775808)).add(jsonNull())
    val before = jsonObject().put("title", jsonString("初🙂\0\"\n"))
    before.put("rows", rows.build()).put("max", jsonInt(9223372036854775807)).put("", jsonObject().build());
    val old = before.build()
    rows.add(jsonBool(true)).add(jsonFloat(-0.0));
    before.put("rows", rows.build()).put("tail", jsonArray().build());
    gcCollect()
    print(old.encode()); print(before.build().encode()); print(old.field("rows").encode())
    print(old.field("rows").item(1).isNull()); print(old.field("absent").missing())
}'''
        expected = ['{"title":"初🙂\\u0000\\\"\\n","rows":[-9223372036854775808,null],"max":9223372036854775807,"":{}}',
                    '{"title":"初🙂\\u0000\\\"\\n","rows":[-9223372036854775808,null,true,-0.0],"max":9223372036854775807,"":{},"tail":[]}',
                    '[-9223372036854775808,null]', 'true', 'true']
        for flags in ([], ['--release']):
            with self.subTest(flags=flags):
                self.assertEqual(self.run_source(source, *flags), expected)

    def test_pretty_and_compact_roundtrip(self):
        lines = self.run_source(r'''
fn render(): Result<Unit,String> {
    val parsed = parseJson(r#"{"a":[0.12345678901234568,"\ud83d\ude42"],"path\\part\n":true}"#)?
    val value = jsonObject().put("payload", parsed).build()
    val pretty = value.encode(pretty: true)
    print((parseJson(pretty)?).encode())
    print(pretty)
    Ok({})
}
fn main(): Unit {
    match render() { Ok(_) => {}, Err(error) => throw Exception(error) }
}''')
        expected = {'payload': {'a': [0.12345678901234568, '🙂'], 'path\\part\n': True}}
        self.assertEqual(json.loads(lines[0]), expected)
        self.assertEqual('\n'.join(lines[1:]), json.dumps(expected, ensure_ascii=False, indent=2))

    def test_invalid_values_do_not_mutate_builders(self):
        lines = self.run_source(r'''
fn main(): Unit {
    val object = jsonObject().put("a", jsonInt(7))
    val array = jsonArray().add(jsonNull())
    val missing = object.build().field("unknown")
    print(try { missing.encode() } catch error: Exception { error.message })
    print(try { object.put("a", missing).build().encode() } catch error: Exception { error.message })
    print(try { array.add(missing).build().encode() } catch error: Exception { error.message })
    print(try { object.put("bad\0key", jsonNull()).build().encode() } catch error: Exception { error.message })
    val zero: Float64 = 0
    for number in [1.0 / zero, -1.0 / zero, zero / zero] {
        print(try { jsonFloat(number).encode() } catch error: Exception { error.message })
    }
    print(object.build().encode()); print(array.build().encode())
}''')
        self.assertEqual(lines, ['JSON cannot encode a missing value; use jsonNull() for null'] * 3 +
                         ['JSON object keys cannot contain NUL: "bad\\u0000key"'] +
                         ['JSON cannot encode a non-finite Float64'] * 3 + ['{"a":7}', '[null]'])

    def test_encoding_matches_parser_nesting_boundary(self):
        lines = self.run_source('''
fn main(): Unit {
    var value = jsonNull()
    for depth in 0..<128 { value = jsonArray().add(value).build() }
    print(match parseJson(value.encode()) { Ok(_) => true, Err(_) => false })
    value = jsonArray().add(value).build()
    print(try { value.encode() } catch error: Exception { error.message })
}''')
        self.assertEqual(lines, ['true', 'JSON nesting exceeds 128 levels'])


if __name__ == '__main__':
    unittest.main()
