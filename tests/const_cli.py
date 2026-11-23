import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()


class Constants(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.app = self.root / 'app'
        self.lib = self.root / 'cfg'
        (self.app / 'src').mkdir(parents=True)
        (self.lib / 'src/internal').mkdir(parents=True)
        self.env = {**os.environ, 'LYRA_CACHE': str(self.root / 'cache')}

    def write(self, relative, text):
        path = self.root / relative
        path.write_text(text, encoding='utf-8', newline='\n')
        return path

    def cli(self, *args):
        return subprocess.run([COMPILER, *args], capture_output=True, text=True, encoding='utf-8', timeout=30, env=self.env)

    def diagnose(self, package, code, fragment):
        result = self.cli('check', package, '--diagnostics=json')
        self.assertNotEqual(result.returncode, 0, result.stdout)
        diagnostic, = json.loads(result.stderr)
        self.assertEqual(diagnostic['code'], code, diagnostic)
        self.assertIn(fragment, diagnostic['message'])
        return diagnostic

    def test_values_are_typed_and_inlined_after_cross_package_reexport(self):
        self.write('app/lyra.json5', '{name:"app",version:"0.1.0",dependencies:{cfg:{name:"cfg",version:"^0.1.0",path:"../cfg"}}}')
        self.write('cfg/lyra.json5', '{name:"cfg",version:"0.1.0",kind:"lib"}')
        self.write('cfg/src/internal/calc.ly', '''const base: Int8 = -2
pub const total: Int = score(from: base, through: 8)
pub const text: String = "值🙂=${total}"
pub const fn score(from: Int, through: Int): Int {
    if from > through { return 0 }
    var sum = 0
    for number in from..through { if number < 0 { continue }; sum += number }
    sum
}
''')
        self.write('cfg/src/api.ly', '''pub import self.internal.total as entries
pub import self.internal.text
pub import self.internal.score
''')
        self.write('app/src/main.ly', '''import cfg.entries as count
import cfg.text
import cfg.score
const maximum: UInt64 = 18446744073709551615
const rounded: Float32 = (16777216.0f32 + 1.0f32) - 16777216.0f32
const ok: Bool = true || (1 / 0 == 0)
const minimum: Int = -9223372036854775808
const widened: Float64 = maximum.toFloat64()
const sizeofText: Int = text.size
fn main(): Unit {
    print(count)
    print(text)
    print(score(1, 3))
    print(maximum)
    print(rounded)
    print(ok)
    print(minimum)
    print(widened > 0.0)
    print(sizeofText)
}
''')
        for mode in ([], ['--release']):
            with self.subTest(mode=mode):
                result = self.cli('run', self.app, *mode)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.splitlines(), ['36', '值🙂=36', '6', '18446744073709551615', '0', 'true', '-9223372036854775808', 'true', '5'])
        self.write('app/src/main.ly', 'import cfg.internal.base\nfn main(): Unit {}\n')
        self.diagnose(self.app, 'LYN0003', 'base')

    def test_cross_module_cycle_reports_the_reference_that_closes_it(self):
        self.write('app/lyra.json5', '{name:"app",version:"0.1.0"}')
        (self.app / 'src/left').mkdir()
        (self.app / 'src/right').mkdir()
        self.write('app/src/left/first.ly', 'import self.right.second\npub const first: Int = second\n')
        self.write('app/src/right/second.ly', 'import self.left.first\npub const second: Int = first\n')
        self.write('app/src/main.ly', 'fn main(): Unit {}\n')
        diagnostic = self.diagnose(self.app, 'LYCT0002', '依赖环')
        self.assertEqual(Path(diagnostic['file']).name, 'second.ly')
        self.assertEqual(diagnostic['range']['start']['line'], 2)
        self.assertEqual(diagnostic['range']['start']['column'], 25)

    def test_purity_checks_unused_branches_and_constant_bindings_are_immutable(self):
        cases = [
            ('fn io(): Int { 2 }\nconst fn decide(): Int { if true { 1 } else { io() } }', 'LYCT0001', 'io'),
            ('const fn bad(): Unit { print(1) }', 'LYCT0001', 'print'),
            ('const fn bad(): List<Int> { [1] }', 'LYCT0001', '标量'),
            ('const value: Int = 1\nfn attempt(): Unit { value = 2 }', 'LYT0004', '赋值'),
            ('const value: Int = 1\nfn attempt(): Int { value() }', 'LYCT0001', '不是函数'),
        ]
        for source, code, message in cases:
            with self.subTest(source=source):
                self.write('app/src/main.ly', source + '\nfn main(): Unit {}')
                self.diagnose(self.app, code, message)

    def test_overflow_and_budgets_are_build_time_errors(self):
        cases = [
            ('const overflow: Int8 = 127 + 1', 'LYCT0003', 'Int8'),
            ('const overflow: UInt = 0 - 1', 'LYCT0003', 'UInt'),
            ('const zero: Int = 1 / 0', 'LYCT0003', '除零'),
            ('const fn bad(): Int { bad() }\nconst value: Int = bad()', 'LYCT0004', '深度'),
            ('const fn bad(): Unit { while true {} }\nconst badLoop: Unit = bad()', 'LYCT0004', '步数'),
        ]
        for source, code, message in cases:
            with self.subTest(source=source):
                self.write('app/src/main.ly', source + '\nfn main(): Unit {}')
                self.diagnose(self.app, code, message)


if __name__ == '__main__':
    unittest.main()
