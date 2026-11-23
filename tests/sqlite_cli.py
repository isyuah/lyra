from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


COMPILER = Path(sys.argv.pop(1)).resolve()


class SQLite(unittest.TestCase):
    def test_prepared_statement_cleanup_and_copied_results(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary)
            (package / "src").mkdir()
            (package / "lyra.json5").write_text("{name:'test/query',version:'1.0.0'}")
            (package / "src" / "main.ly").write_text(r'''
import std.io.assert
import std.sqlite.open
import std.sqlite.openReadOnly
fn main(): Unit {
    val bad = openReadOnly("does-not-exist.db")
    assert(!bad.status().ok() && bad.status().code() != 0, "missing file")
    assert(!bad.prepare("SELECT 1").status().ok(), "failed open")
    val db = open(":memory:")
    assert(db.status().ok(), "open")
    val create = db.prepare("CREATE TABLE people(id INTEGER PRIMARY KEY, name TEXT)")
    assert(create.next().done() && create.close().ok(), "table")
    val insert = db.prepare("INSERT INTO people VALUES(?1,?2)")
    assert(insert.bindInt(1, 7).ok() && insert.bindText(2, "李\0Ada").ok(), "parameters")
    assert(insert.next().done() && insert.close().ok(), "insert")
    val query = db.prepare("SELECT name, id, NULL, '' FROM people WHERE id=?1")
    assert(query.bindInt(1, 7).ok() && query.next().row(), "row")
    val text = query.readText(0)
    assert(text.status.ok() && query.readInt(1).value == 7, "typed reads")
    assert(query.readText(2).status.isNull() && query.readText(3).status.ok(), "null vs empty")
    assert(!query.readInt(0).status.ok() && !query.readInt(4294967296).status.ok(), "invalid column")
    assert(db.close().busy() && query.next().done(), "active statement")
    assert(query.close().ok() && query.close().ok(), "finalize")
    val invalidText = db.prepare("SELECT CAST(x'ff' AS TEXT)")
    assert(invalidText.next().row(), "read invalid text")
    assert(!invalidText.readText(0).status.ok() && invalidText.close().ok(), "validate SQLite UTF-8")
    val invalid = db.prepare("malformed query")
    assert(!invalid.status().ok() && invalid.status().operation() == "prepare", "SQL diagnostic")
    assert(invalid.status().message() != "" && !invalid.next().row(), "failed statement")
    assert(db.close().ok() && db.close().ok(), "close")
    gcCollect()
    print(text.value)
}
''', encoding="utf-8")
            for flags in ([], ["--release"]):
                with self.subTest(release=bool(flags)):
                    app = package / "query"
                    built = subprocess.run([COMPILER, "build", package, "-o", app, *flags], capture_output=True, timeout=30)
                    self.assertEqual(built.returncode, 0, built.stderr.decode())
                    result = subprocess.run([app], cwd=package, capture_output=True, timeout=10)
                    self.assertEqual((result.returncode, result.stdout, result.stderr), (0, "李\0Ada\n".encode(), b""))
                    dynamic = subprocess.check_output(["readelf", "-dW", app], text=True)
                    dependencies = {line.split("[")[1].split("]")[0] for line in dynamic.splitlines() if "(NEEDED)" in line}
                    self.assertEqual(dependencies, {"libc.so.6", "libsqlite3.so.0"})


if __name__ == "__main__":
    unittest.main()
