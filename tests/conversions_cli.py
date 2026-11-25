import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()


class Conversions(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.src = self.root / 'src'
        self.src.mkdir()
        (self.root / 'lyra.json5').write_text('{name:"app", version:"1.0.0"}')

    def source(self, name, text):
        path = self.src / name
        path.parent.mkdir(exist_ok=True, parents=True)
        path.write_text(text)

    def cli(self, *args):
        return subprocess.run([COMPILER, *args], capture_output=True, text=True, timeout=30)

    def reject(self, code):
        result = self.cli('check', self.root, '--diagnostics=json')
        self.assertNotEqual(result.returncode, 0)
        error, = json.loads(result.stderr)
        self.assertEqual(error['code'], code, error)
        return error

    def runBoth(self, output):
        for profile in ([], ['--release']):
            result = self.cli('run', self.root, *profile)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), output)

    def test_direct_return_generic_arguments_and_result_propagation(self):
        self.source('data/item.ly', '''import self.reply.{Response}
pub class User(pub val name: String)
pub implicit (user: User): Response { print("convert"); Response(user.name) }
''')
        self.source('reply/result.ly', 'pub struct Response(pub val message: String)')
        self.source('main.ly', '''import self.data.{User as Person}
import self.reply.{Response}
fn identity<T>(value: T): T { print("identity"); value }
fn implicitReturn(user: Person): Response { identity(user) }
fn explicitArgument(user: Person): Response { identity<Response>(user) }
fn read(user: Person, ok: Bool): Result<Int,Person> { if ok { Ok(9) } else { Err(user) } }
fn request(user: Person, ok: Bool): Result<Int,Response> { Ok(read(user, ok)?) }
fn main(): Unit {
    val user = Person("Ada")
    print(implicitReturn(user).message)
    print(explicitArgument(user).message)
    print(match request(user, true) { Ok(number) => number, Err(_) => 0 })
    print(match request(user, false) { Ok(_) => "unexpected", Err(reason) => reason.message })
}''')
        self.runBoth(['identity', 'convert', 'Ada', 'convert', 'identity', 'Ada', '9', 'convert', 'Ada'])

    def test_visibility_duplicate_orphan_and_ambiguity(self):
        model = 'import self.web.{Response}\npub struct User(pub val id: Int)\n'
        web = 'import self.model.{User}\npub struct Response(pub val id: Int)\n'
        conversion = 'implicit (user: User): Response { Response(user.id) }\n'
        main = 'import self.model.{User}\nimport self.web.{Response}\nfn send(user: User): Response { user }\nfn main(): Unit {}\n'
        self.source('model/model.ly', model + conversion)
        self.source('web/web.ly', web)
        self.source('main.ly', main)
        self.reject('LYT0001')
        self.source('main.ly', main + conversion)
        self.reject('LYV0001')
        self.source('main.ly', main)
        self.source('model/model.ly', model + 'pub ' + conversion + conversion)
        self.reject('LYV0001')
        self.source('model/model.ly', model + 'pub ' + conversion)
        self.source('web/web.ly', web + 'pub ' + conversion)
        error = self.reject('LYV0002')
        self.assertTrue(error['file'].endswith('main.ly'))
        for name in ('model.ly', 'web.ly'):
            self.assertIn(name + ':3', error['message'])

    def test_no_chaining_or_blanket_result_conversion(self):
        self.source('main.ly', '''struct User()
struct Middle()
struct Response()
implicit (value: User): Middle { Middle() }
implicit (value: Middle): Response { Response() }
fn read(): Result<Int,User> { Err(User()) }
fn fail(): Result<Int,Response> { Ok(read()?) }
fn main(): Unit {}''')
        self.assertEqual(self.reject('LYT0013')['range']['start']['line'], 7)


if __name__ == '__main__':
    unittest.main()
