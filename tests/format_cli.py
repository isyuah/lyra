import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()


class FormatCli(unittest.TestCase):
    def command(self, *arguments):
        return subprocess.run([COMPILER, *arguments], capture_output=True, timeout=30)

    def test_preserves_literals_comments_newlines_and_generic_call_adjacency(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary)
            source = package / 'src' / '中文 name.ly'
            source.parent.mkdir()
            lines = [
                'fn identity<T>(value: T): T { value }',
                'fn main(): Unit {  ',
                '\tval message = r#"first  ',
                ' keep\t  ',
                'last"#  ',
                '\t/* comment  ',
                '  keep spacing */ ',
                '\tprint(message) ',
                '\tprint(identity<Int>(',
                ' 42',
                ' )) ',
                '}',
            ]
            original = '\r\n'.join(lines).encode('utf-8')
            source.write_bytes(original)
            before = self.command('run', package)
            self.assertEqual(before.returncode, 0, before.stderr)

            report = self.command('fmt', '--check', source, '--diagnostics=json')
            self.assertNotEqual(report.returncode, 0)
            diagnostic, = json.loads(report.stderr)
            self.assertEqual(diagnostic['code'], 'LYFMT0001')
            self.assertEqual(source.read_bytes(), original)
            result = self.command('fmt', source)
            self.assertEqual(result.returncode, 0, result.stderr)
            expected = lines.copy()
            expected[1] = 'fn main(): Unit {'
            expected[2] = '    val message = r#"first  '
            expected[4] = 'last"#'
            expected[5] = '    /* comment  '
            expected[6] = '  keep spacing */'
            expected[7:11] = ['    print(message)', '    print(identity<Int>(', '        42', '    ))']
            self.assertEqual(source.read_bytes(), ('\r\n'.join(expected) + '\r\n').encode('utf-8'))
            self.assertEqual(self.command('fmt', '--check', source).returncode, 0)
            after = self.command('run', package)
            self.assertEqual((after.returncode, after.stdout, after.stderr), (0, before.stdout, b''))

    def test_rejects_invalid_literal_without_writing(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'broken.ly'
            original = b'fn main(): Unit { r#"no closing delimiter\n'
            source.write_bytes(original)
            result = self.command('fmt', source, '--diagnostics=json')
            self.assertNotEqual(result.returncode, 0)
            diagnostic, = json.loads(result.stderr)
            self.assertEqual(diagnostic['code'], 'LYL0003')
            self.assertEqual(source.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
