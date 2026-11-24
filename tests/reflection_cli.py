import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from platform_cli import executable


COMPILER = Path(sys.argv.pop(1)).resolve()


class Reflection(unittest.TestCase):
    def test_public_fields_keep_runtime_type(self):
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory)
            (package / 'src').mkdir()
            (package / 'lyra.json5').write_text('{name:"reflection",version:"0.1.0"}', encoding='utf-8')
            source = package / 'src/main.ly'
            source.write_text('''
import std.reflect.typeName
import std.reflect.fieldCount
import std.reflect.fieldNames
import std.reflect.field
struct User(pub val name: String, pub val age: Int, val secret: String)
class Account(pub val id: Int)
fn main(): Unit {
    val user: Any = User("Ada", 37, "hidden")
    print(typeName(user))
    print(fieldCount(user))
    for name in fieldNames(user) { print(name) }
    print(match field(user, "age") {
        Some(value) => value as? Int ?? -1,
        None => -1
    })
    print(match field(user, "secret") {
        Some(_) => "visible",
        None => "private"
    })
    val account: Any = Account(9)
    print(match field(account, "id") {
        Some(value) => value as? Int ?? -1,
        None => -1
    })
}
''', encoding='utf-8')
            for release in (False, True):
                output = executable(package / ('release' if release else 'debug'))
                build = subprocess.run([COMPILER, 'build', package, *(['--release'] if release else []), '-o', output],
                                       capture_output=True, text=True, timeout=45)
                self.assertEqual(build.returncode, 0, build.stderr)
                run = subprocess.run([output], capture_output=True, text=True, timeout=10)
                self.assertEqual((run.returncode, run.stdout, run.stderr),
                                 (0, 'User\n2\nname\nage\n37\nprivate\n9\n', ''))

    def test_cross_package_reflection_only_lists_public_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            library = root / 'models'
            (library / 'src').mkdir(parents=True)
            (library / 'lyra.json5').write_text(
                '{name:"models",version:"0.1.0",kind:"lib"}', encoding='utf-8')
            (library / 'src/user.ly').write_text('''
pub struct User(pub val name: String, val secret: String)
pub fn createUser(): User { User("Ada", "hidden") }
''', encoding='utf-8')

            package = root / 'app'
            (package / 'src').mkdir(parents=True)
            (package / 'lyra.json5').write_text(
                '{name:"reflection-app",version:"0.1.0",dependencies:{model:{name:"models",version:"^0.1.0",path:"../models"}}}',
                encoding='utf-8')
            source = package / 'src/main.ly'
            source.write_text('''
import model.createUser
import std.reflect.typeName
import std.reflect.fieldCount
import std.reflect.fieldNames
import std.reflect.field
fn main(): Unit {
    val user: Any = createUser()
    print(typeName(user))
    print(fieldCount(user))
    for name in fieldNames(user) { print(name) }
    print(match field(user, "name") {
        Some(value) => value as? String ?? "wrong type",
        None => "missing"
    })
    print(match field(user, "secret") {
        Some(_) => "private field leaked",
        None => "private"
    })
}
''', encoding='utf-8')

            output = executable(package / 'debug')
            build = subprocess.run([COMPILER, 'build', package, '-o', output],
                                   capture_output=True, text=True, timeout=45)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([output], capture_output=True, text=True, timeout=10)
            self.assertEqual((run.returncode, run.stdout, run.stderr),
                             (0, 'User\n1\nname\nAda\nprivate\n', ''))


if __name__ == '__main__':
    unittest.main()
