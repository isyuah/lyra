import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from platform_cli import executable


COMPILER = Path(sys.argv.pop(1)).resolve()


class GenericTasks(unittest.TestCase):
    def test_nested_tasks_across_a_module_and_a_generic_declaration(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'src/model').mkdir(parents=True)
            (root / 'lyra.json5').write_text('{name: "app", version: "1.0.0"}')
            (root / 'src/model/cell.ly').write_text('pub class Cell(pub val value: Int)\n')
            (root / 'src/main.ly').write_text('''import self.model.Cell as Box
async fn make(): Box { Box(17) }
async fn nested(): Task<Box> { make() }
async fn flatten<T>(work: Task<Task<T>>): T { await (await work) }
fn forward<U>(work: Task<Task<U>>): Task<Task<U>> {
    val local: Task<Task<U>> = work
    local
}
async fn main(): Unit { print((await flatten(forward(nested()))).value) }
''')
            for flags in ([], ['--release']):
                exe = executable(root / 'app')
                result = subprocess.run([COMPILER, 'build', root, '-o', exe, *flags], capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(subprocess.check_output([exe], timeout=10), b'17\n')
            (root / 'src/main.ly').write_text('''async fn take<T>(work: Task<T>): T { await work }
async fn main(): Unit { print(await take(9)) }
''')
            result = subprocess.run([COMPILER, 'check', root, '--diagnostics=json'], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            error, = json.loads(result.stderr)
            self.assertEqual(error['code'], 'LYT0001')
            self.assertEqual((error['expected'], error['actual']), ('Task<T>', 'Int'))


if __name__ == '__main__':
    unittest.main()
