import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()


class Overloads(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.app = self.root / 'app'
        self.write('app/lyra.json5', '{name:"app", version:"0.1.0"}')

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(exist_ok=True, parents=True)
        path.write_text(text, encoding='utf-8')

    def main(self, text):
        self.write('app/src/main.ly', text)

    def cli(self, *args):
        return subprocess.run([COMPILER, args[0], self.app, *args[1:]], capture_output=True, text=True, encoding='utf-8', timeout=30)

    def runBoth(self, *lines):
        for profile in ([], ['--release']):
            result = self.cli('run', *profile)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), list(lines))

    def reject(self, code, line=None, column=None):
        result = self.cli('check', '--diagnostics=json')
        self.assertNotEqual(result.returncode, 0)
        diagnostic, = json.loads(result.stderr)
        self.assertEqual(diagnostic['code'], code, diagnostic)
        start = diagnostic['range']['start']
        if line is not None:
            self.assertEqual(start['line'], line, diagnostic)
        if column is not None:
            self.assertEqual(start['column'], column, diagnostic)
        return diagnostic['message']

    def conversionModules(self, competing=False):
        self.write('app/src/model/user.ly', '''import self.web.{Response}
pub struct User(pub val id: Int)
pub implicit (user: User): Response { print("model"); Response(user.id) }
''')
        conversion = 'pub implicit (user: User): Response { print("web"); Response(user.id) }\n' if competing else ''
        self.write('app/src/web/response.ly', 'import self.model.{User}\npub struct Response(pub val id: Int)\n' + conversion)
        return 'import self.model.{User}\nimport self.web.{Response}\n'

    def test_direct_conversion_runs_only_when_selected(self):
        imports = self.conversionModules(competing=True)
        response = 'fn send(value: Response): String { "response" }\n'
        user = 'fn send(value: User): String { "user" }\n'
        body = 'fn main(): Unit { val user = User(7); print(send(user)) }\n'
        self.main(imports + response + user + body)
        self.runBoth('user')
        self.main(imports + response + 'fn send(value: String): String { "string" }\n' + body)
        message = self.reject('LYV0002', 5, body.index('send(user)') + len('send(') + 1)
        self.assertIn('user.ly:3', message)
        self.assertIn('response.ly:3', message)
        self.main(imports + response + body.replace('send(user)', 'send(Response(user.id))'))
        self.runBoth('response')
        self.conversionModules()
        self.main(imports + response + body)
        self.runBoth('model', 'response')

    def test_selects_by_each_argument_not_conversion_count(self):
        imports = self.conversionModules()
        left = 'fn send(first: User, second: Response): String { "left" }\n'
        right = 'fn send(first: Response, second: User): String { "right" }\n'
        call = 'fn main(): Unit { val user = User(0); print(send(user, user)) }'
        for declarations in (left + right, right + left):
            self.main(imports + declarations + call)
            message = self.reject('LYT0019', 5, call.index('send(') + 1)
            self.assertRegex(message, r'send\(User, Response\).*实参：2')
            self.assertRegex(message, r'send\(Response, User\).*实参：1')
        self.main(imports + left + right + 'fn send(first: User, second: User): String { "both" }\n' + '''fn main(): Unit {
    val user = User(7)
    val response = Response(7)
    print(send(user, user))
    print(send(user, response))
    print(send(response, user))
}''')
        self.runBoth('both', 'left', 'right')
        self.main(imports + '''fn send(first: User, second: Response, third: Response): Unit {}
fn send(first: Response, second: User, third: User): Unit {}
fn main(): Unit { val user = User(0); send(user, user, user) }''')
        message = self.reject('LYT0019', 5)
        self.assertRegex(message, r'send\(User, Response, Response\).*实参：2、3')
        self.assertRegex(message, r'send\(Response, User, User\).*实参：1')

    def test_builtin_and_generic_precedence(self):
        imports = self.conversionModules()
        self.main(imports + '''struct Count(val value: Int64)
implicit (number: Int32): Count { print("unexpected"); Count(number) }
fn write(value: Int64): String { "widen" }
fn write(value: Count): String { "convert" }
fn send(value: User): String { "exact" }
fn send<Value>(value: Value): String { "generic" }
fn reply(value: Response): String { "response" }
fn reply<Value>(value: Value): String { "generic" }
fn main(): Unit {
    val value: Int32 = 12
    print(write(value))
    print(send(User(7)))
    print(reply(User(7)))
}
''')
        self.runBoth('widen', 'exact', 'generic')
        options = '''fn write(value: Int32?): String { "nullable" }
fn write(value: Int64, extra: Int = 0): String { "widen" }
'''
        call = 'fn main(): Unit { val value: Int32 = 12; print(write(value)) }'
        self.main(options + call)
        self.reject('LYT0019', 3)
        self.main(options + 'fn write(value: Int32): String { "exact" }\n' + call)
        self.runBoth('exact')

    def test_literal_fixed_spread_and_default_tiebreaks(self):
        self.main('''fn mark(value: String): Int { print(value); 0 }
fn width(value: Int8): String { "int8" }
fn width(value: Int16): String { "int16" }
fn shape(value: Int): String { "fixed" }
fn shape(values: Int...): String { "variadic" }
fn shape(value: Int8): String { "narrow" }
fn defaults(value: Int, second: Int = mark("chosen")): String { "short" }
fn defaults(value: Int, second: Int = mark("ignored"), third: Int = 0): String { "long" }
fn main(): Unit {
    print(width(200))
    print(shape(1))
    print(shape(...[1, 2]))
    print(defaults(1))
}''')
        self.runBoth('int16', 'fixed', 'variadic', 'chosen', 'short')
        self.main('''fn width(value: Int8): Unit {}
fn width(value: Int16, extra: Int = 0): Unit {}
fn main(): Unit { width(1) }''')
        self.reject('LYT0019', 3)

    def test_candidate_checks_do_not_change_closure_captures(self):
        self.main('''struct Box<Value>(val item: Value, val seed: Int = 7)
fn select(value: Int): Int { value }
fn select(value: String): Int { value.size }
fn use(priority: Int?, action: (String) -> Int): Int { action("aba") }
fn use(priority: Int, action: (Int) -> Int): Int { action(30) }
fn main(): Unit {
    val local = 5
    print(use(1) { select(it) + Box(it).seed + local })
}''')
        self.runBoth('42')

    def test_function_values_and_duplicate_signatures(self):
        definitions = '''fn read(value: String): Int { value.size }
fn read<Value>(value: Value): Value { value }
fn read(value: Int): Int { value + 1 }
'''
        self.main(definitions + '''fn main(): Unit {
    val action: (Int) -> Int = read
    val flag: (Bool) -> Bool = read
    print(action(41))
    print(flag(true))
}''')
        self.runBoth('42', 'true')
        self.main(definitions + 'fn main(): Unit { val action = read }')
        self.reject('LYT0019', 4)
        for definitions in (
            'fn bad(value: Int): Int { value }\nfn bad(other: Int64): Unit {}',
            'fn bad<T>(value: T): T { value }\nfn bad<U>(other: U): U { other }',
            'fn main(): Unit {}\nfn main(value: Int): Unit {}',
        ):
            self.main(definitions + ('\nfn main(): Unit {}' if 'fn main' not in definitions else ''))
            self.reject('LYN0002', 2)

    def test_reexport_exposes_only_public_candidates(self):
        self.write('lib/lyra.json5', '{name:"lib", version:"0.1.0", kind:"lib"}')
        self.write('app/lyra.json5', '''{name:"app", version:"0.1.0", dependencies:{lib:{name:"lib", version:"^0.1.0", path:"../lib"}}}''')
        self.write('lib/src/lib.ly', 'pub import self.impl.{pick as select}\n')
        self.write('lib/src/impl/pick.ly', '''fn pick(value: Int): String { "private" }
pub fn pick(value: Int?): String { "public nullable" }
pub fn pick(value: String): String { "public string" }
pub fn internal(): String { pick(1) }
''')
        self.main('''import lib.{select as choose}
import lib.impl.{internal}
fn main(): Unit {
    print(choose(1))
    print(choose("text"))
    print(internal())
}''')
        self.runBoth('public nullable', 'public string', 'private')
        self.main('''import lib.{select as choose}
fn main(): Unit { val callback: (Int) -> String = choose }''')
        self.assertNotIn('choose(Int) @', self.reject('LYT0019', 2))


if __name__ == '__main__':
    unittest.main()
