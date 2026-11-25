import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()


class JsonCodecs(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        self.app = root / 'app'
        self.main = self.app / 'src/main.ly'
        self.main.parent.mkdir(parents=True)
        self.env = {**os.environ, 'LYRA_CACHE': str(root / 'cache')}
        lib = root / 'models'
        (lib / 'src').mkdir(parents=True)
        (lib / 'lyra.json5').write_text(json.dumps(dict(name='models', version='0.1.0', kind='lib')))
        (self.app / 'lyra.json5').write_text(json.dumps(dict(name='app', version='0.1.0', dependencies={
            'model': dict(name='models', version='^0.1.0', path='../models')})))
        (lib / 'src/user.ly').write_text('''import std.json.{JsonCodec}
#derive(Json)
pub struct User([jsonName("display-name")] pub val name: String = "guest", val secret: String = "private")
pub fn wire(): JsonCodec<User> { jsonCodecUser() }
#derive(Json)
pub struct Page<T>(pub val items: List<T>, pub val total: Int = items.size)
pub fn pageWire<T>(item: JsonCodec<T>): JsonCodec<Page<T>> { jsonCodecPage(item) }
''', encoding='utf-8')

    def command(self, action, *flags):
        return subprocess.run([COMPILER, action, self.app, *flags], capture_output=True, timeout=40, env=self.env)

    def test_public_codec_with_type_and_factory_aliases(self):
        self.main.write_text(r'''import model.{User as JsonValue}
import model.{wire as value}
import model.{wire as intText}
import std.json.{parseJson}
#derive(Json)
struct Batch([jsonName("user.rows")][jsonCodec("value")] pub val rows: List<List<JsonValue?>?>? = None,
             [jsonCodec("intText")] pub val delta: Option<JsonValue?>)
fn show(text: String, loose: Bool): String {
    val codec = jsonCodecBatch()
    val result = if loose { codec.decodeIgnoringUnknownFields(text) } else { codec.decode(text) }
    match result { Ok(batch) => codec.encode(batch), Err(error) => error }
}
fn directRead(text: String, loose: Bool): Result<String,String> {
    val node = parseJson(text)?
    val field = if loose { node.ignoringUnknownFields() } else { node }
    Ok(value().encode(value().read(field, "$.user")?))
}
fn main(): Unit {
    val codec = value()
    print(match codec.decode("{}") { Ok(user) => codec.encode(user), Err(error) => error })
    val nested = r#"{"user.rows":[null,[null,{"display-name":"Ada","next":0}]],"delta":null}"#
    print(show(nested, false)); print(show(nested, true))
    print(show(r#"{"user.rows":[[null,{"display-name":4}]]}"#, true))
    print(show("{}", false)); print(show(r#"{"delta":{}}"#, false))
    print(match directRead(r#"{"extra":true}"#, false) { Ok(text) => text, Err(error) => error })
    print(match directRead(r#"{"extra":true}"#, true) { Ok(text) => text, Err(error) => error })
}''', encoding='utf-8')
        expected = ['{"display-name":"guest"}', '$["user.rows"][1][1].next: unknown field',
                    '{"user.rows":[null,[null,{"display-name":"Ada"}]],"delta":null}',
                    '$["user.rows"][0][1]["display-name"]: expected String', '{}',
                    '{"delta":{"display-name":"guest"}}', '$.user.extra: unknown field', '{"display-name":"guest"}']
        for flags in ([], ['--release', '--locked']):
            with self.subTest(flags=flags):
                run = self.command('run', *flags)
                self.assertEqual(run.returncode, 0, run.stderr.decode())
                self.assertEqual(run.stdout.decode().splitlines(), expected)

    def test_handwritten_codec_composes_and_preserves_paths(self):
        self.main.write_text(r'''import std.json.{JsonCodec}
import std.json.{jsonCodec}
import std.json.{quoteJson}
struct Label(pub val text: String)
fn labelWire(): JsonCodec<Label> {
    jsonCodec<Label>((item) => { quoteJson(item.text) }, (node, path) => { Ok(Label(node.readString(path)?)) })
}
#derive(Json)
struct Card([jsonCodec("labelWire")] pub val labels: List<Label> = [])
fn main(): Unit {
    val codec = jsonCodecCard()
    print(codec.encode(Card([Label("A")])))
    print(match codec.decode("{}") { Ok(card) => card.labels.size.toString(), Err(error) => error })
    print(match codec.decode(r#"{"labels":["ok",false]}"#) { Ok(_) => "accepted", Err(error) => error })
}''', encoding='utf-8')
        run = self.command('run')
        self.assertEqual(run.returncode, 0, run.stderr.decode())
        self.assertEqual(run.stdout.decode().splitlines(), ['{"labels":["A"]}', '0', '$.labels[1]: expected String'])

    def test_composed_codecs_keep_paths_and_nested_sum_states(self):
        self.main.write_text(r'''import model.{wire as userWire}
import std.collections.{stringMap}
import std.json.{jsonCodecInt}
import std.json.{jsonCodecString}
import std.json.{jsonCodecList}
import std.json.{jsonCodecOption}
import std.json.{jsonCodecResult}
import std.json.{jsonCodecStringMap}

fn main(): Unit {
    val codec = jsonCodecStringMap(jsonCodecList(userWire()))
    val unknown = r#"{"组.一":[{"display-name":"Ada","extra":1}]}"#
    print(match codec.decode(unknown) { Ok(_) => "accepted", Err(error) => error })
    print(match codec.decodeIgnoringUnknownFields(unknown) { Ok(value) => codec.encode(value), Err(error) => error })
    print(match codec.decode(r#"{"users":[{"display-name":3}]}"#) { Ok(_) => "accepted", Err(error) => error })

    val nested = jsonCodecOption(jsonCodecOption(jsonCodecInt()))
    val someNone: Option<Option<Int>> = Some(None)
    print(nested.encode(someNone))
    print(match nested.decode(nested.encode(someNone)) { Ok(value) => value == someNone, Err(_) => false })
    print(match nested.decode(r#"{"tag":"Some"}"#) { Ok(_) => "accepted", Err(error) => error })
    print(match nested.decode(r#"{"tag":"None","value":0}"#) { Ok(_) => "accepted", Err(error) => error })

    val resultCodec = jsonCodecResult(jsonCodecInt(), jsonCodecString())
    val error: Result<Int,String> = Err("offline")
    print(resultCodec.encode(error))
    print(match resultCodec.decode(resultCodec.encode(error)) { Ok(value) => value == error, Err(_) => false })
    print(match jsonCodecInt().decode("9223372036854775807") { Ok(value) => value.toString(), Err(error) => error })
    val badKeys = stringMap<Int>()
    badKeys.put("bad\0key", 1);
    print(try { jsonCodecStringMap(jsonCodecInt()).encode(badKeys) } catch problem: Exception { problem.message })
}''', encoding='utf-8')
        expected = ['$["组.一"][0].extra: unknown field', '{"组.一":[{"display-name":"Ada"}]}',
                    '$.users[0]["display-name"]: expected String', '{"tag":"Some","value":{"tag":"None"}}',
                    'true', '$.value: missing field', '$.value: unknown field', '{"tag":"Err","value":"offline"}',
                    'true', '9223372036854775807', 'JSON object keys cannot contain NUL: "bad\\u0000key"']
        for flags in ([], ['--release', '--locked']):
            with self.subTest(flags=flags):
                run = self.command('run', *flags)
                self.assertEqual(run.returncode, 0, run.stderr.decode())
                self.assertEqual(run.stdout.decode().splitlines(), expected)

    def test_codec_type_and_export_are_checked(self):
        for source, code, reason in (
            ('import model.{jsonCodecUser}\nfn main(): Unit {}', 'LYN0003', 'jsonCodecUser'),
            ('import model.{wire}\n#derive(Json)\nstruct Wrong([jsonCodec("wire")] pub val items: List<Int?>)\nfn main(): Unit {}',
             'LYT0001', 'JsonCodec'),
            ('#derive(Json)\nstruct Wrong([jsonCodec("unknown")] pub val id: Int)\nfn main(): Unit {}',
             'LYN0001', 'unknown'),
            ('#derive(Json)\nstruct Wrong([jsonCodec("hack`{}")] pub val id: Int)\nfn main(): Unit {}',
             'LYM0002', 'factory identifier'),
        ):
            with self.subTest(reason=reason):
                self.main.write_text(source, encoding='utf-8')
                result = self.command('check', '--diagnostics=json')
                self.assertNotEqual(result.returncode, 0)
                diagnostic, = json.loads(result.stderr)
                self.assertEqual(diagnostic['code'], code, diagnostic)
                self.assertIn(reason, diagnostic['message'])

    def test_generic_records_and_enums_roundtrip_across_packages(self):
        self.main.write_text(r'''import model.{Page as RemotePage}
import model.{User}
import model.{pageWire as remoteWire}
import model.{wire as userWire}
import std.collections.{HashMap as Table}
import std.json.{jsonCodecString}
import std.json.{JsonCodec}
import std.json.{jsonCodecInt}
type Count = Int
type Owners = RemotePage<User>
#derive(Json)
enum State<T> { Active, Banned(T), Pair(id: Count, note: T), `else` }
#derive(Json)
struct Batch<T>([jsonCodec("ownersWire")] pub val owners: Owners,
                pub val statuses: List<State<T>>, pub val outcomes: Table<String,Result<T,String?>>)
fn ownersWire(): JsonCodec<Owners> { remoteWire(userWire()) }

fn show(text: String, loose: Bool): String {
    val codec = jsonCodecBatch(jsonCodecString())
    val decoded = if loose { codec.decodeIgnoringUnknownFields(text) } else { codec.decode(text) }
    match decoded { Ok(value) => codec.encode(value), Err(error) => error }
}

fn main(): Unit {
    val states = jsonCodecState(jsonCodecInt())
    print(states.encode(State<Int>.Active))
    print(states.encode(State<Int>.Banned(7)))
    print(states.encode(State<Int>.`else`))
    print(show(r#"{"owners":{"items":[{}]},"statuses":[{"tag":"Pair","value":[3,"ok"]}],"outcomes":{"build":{"tag":"Err","value":{"tag":"None"}}}}"#, false))
    print(show(r#"{"owners":{"items":[{"extra":1}]},"statuses":[],"outcomes":{}}"#, false))
    print(show(r#"{"owners":{"items":[{"extra":1}]},"statuses":[],"outcomes":{}}"#, true))
    print(show(r#"{"owners":{"items":[]},"statuses":[{"tag":"Pair","value":[3,true]}],"outcomes":{}}"#, false))
    print(show(r#"{"owners":{"items":[]},"statuses":[{"tag":"Banned"}],"outcomes":{}}"#, false))
    print(show(r#"{"owners":{"items":[]},"statuses":[{"tag":"unknown"}],"outcomes":{}}"#, false))
}''', encoding='utf-8')
        expected = ['{"tag":"Active"}', '{"tag":"Banned","value":7}', '{"tag":"else"}',
                    '{"owners":{"items":[{"display-name":"guest"}],"total":1},"statuses":[{"tag":"Pair","value":[3,"ok"]}],"outcomes":{"build":{"tag":"Err","value":{"tag":"None"}}}}',
                    '$.owners.items[0].extra: unknown field',
                    '{"owners":{"items":[{"display-name":"guest"}],"total":1},"statuses":[],"outcomes":{}}',
                    '$.statuses[0].value[1]: expected String', '$.statuses[0].value: missing field', '$.statuses[0].tag: unknown State tag "unknown"']
        for flags in ([], ['--release', '--locked']):
            with self.subTest(flags=flags):
                run = self.command('run', *flags)
                self.assertEqual(run.returncode, 0, run.stderr.decode())
                self.assertEqual(run.stdout.decode().splitlines(), expected)


if __name__ == '__main__':
    unittest.main()
