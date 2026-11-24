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
        (lib / 'src/user.ly').write_text('''import std.json.JsonCodec
#derive(Json)
pub struct User([jsonName("display-name")] pub val name: String = "guest", val secret: String = "private")
pub fn wire(): JsonCodec<User> { jsonCodecUser() }
''', encoding='utf-8')

    def command(self, action, *flags):
        return subprocess.run([COMPILER, action, self.app, *flags], capture_output=True, timeout=40, env=self.env)

    def test_public_codec_with_type_and_factory_aliases(self):
        self.main.write_text(r'''import model.User as JsonValue
import model.wire as value
import model.wire as intText
import std.json.parseJson
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
        self.main.write_text(r'''import std.json.JsonCodec
import std.json.jsonCodec
import std.json.quoteJson
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

    def test_codec_type_and_export_are_checked(self):
        for source, code, reason in (
            ('import model.jsonCodecUser\nfn main(): Unit {}', 'LYN0003', 'jsonCodecUser'),
            ('import model.wire\n#derive(Json)\nstruct Wrong([jsonCodec("wire")] pub val items: List<Int?>)\nfn main(): Unit {}',
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


if __name__ == '__main__':
    unittest.main()
