from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from platform_cli import assert_linkage, executable


COMPILER = Path(sys.argv.pop(1)).resolve()


class SQLite(unittest.TestCase):
    def test_prepared_statement_cleanup_and_copied_results(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary)
            (package / "src").mkdir()
            (package / "lyra.json5").write_text("{name:'test/query',version:'1.0.0'}")
            (package / "src" / "main.ly").write_text(r'''
import std.io.assert
import std.sqlite.Busy
import std.sqlite.Connection
import std.sqlite.DbError
import std.sqlite.DbErrorKind
import std.sqlite.open
import std.sqlite.openReadOnly

fn require<T>(result: Result<T, DbError>): T {
    match result { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

fn failure<T>(result: Result<T, DbError>, operation: String): DbError {
    match result {
        Ok(_) => throw Exception("expected SQLite failure"),
        Err(error) => {
            assert(error.operation == operation && error.code != 0 && error.message() != "", "missing error details")
            error
        },
    }
}

fn insertPerson(db: Connection): Result<Unit, DbError> {
    val insert = db.prepare("INSERT INTO people VALUES(?1,?2)")?
    defer insert.close()
    insert.bindInt(1, 7)?
    insert.bindText(2, "李\0Ada")?
    assert(!insert.next()?, "insert unexpectedly returned a row")
    Ok({})
}

fn main(): Unit {
    val missingFile = failure(openReadOnly("does-not-exist.db"), "open")
    assert(match missingFile.kind { DbErrorKind.Sqlite => true, _ => false }, "missing file")
    val db = require(open(":memory:"))
    val alias = db
    val missingTable = failure(db.prepare("SELECT * FROM missing"), "prepare")
    assert(match missingTable.kind { DbErrorKind.Sqlite => true, _ => false }, "missing table")
    val create = require(db.prepare("CREATE TABLE people(id INTEGER PRIMARY KEY, name TEXT)"))
    assert(!require(create.next()), "table")
    create.close()
    require(insertPerson(db))
    val duplicate = failure(insertPerson(db), "next")
    assert(match duplicate.kind { DbErrorKind.Sqlite => true, _ => false }, "step error stayed a Result after defer")
    val missingRow = require(db.prepare("SELECT name FROM people WHERE id=8"))
    assert(!require(missingRow.next()) && !require(missingRow.next()), "no row is success")
    missingRow.close()
    val query = require(db.prepare("SELECT name, id, NULL, '' FROM people WHERE id=?1"))
    val queryAlias = query
    require(query.bindInt(1, 7))
    assert(require(query.next()), "row")
    val text = match require(query.readText(0)) { Some(value) => value, None => throw Exception("missing text") }
    assert(match require(query.readInt(1)) { Some(7) => true, _ => false }, "typed reads")
    assert(require(query.readText(2)) == null && require(query.readInt(2)) == null, "SQL NULL is empty Option")
    assert(match require(query.readText(3)) { Some("") => true, _ => false }, "empty text is not SQL NULL")
    val wrongType = failure(query.readInt(0), "readInt")
    assert(match wrongType.kind { DbErrorKind.Invalid => true, _ => false }, "invalid storage type")
    failure(query.readInt(4294967296), "readInt");
    val busy = try { alias.close(); false } catch error: Busy { error.message != "" }
    assert(busy && !require(query.next()), "busy close kept connection open for statement")
    query.close()
    queryAlias.close()
    val closedQuery = failure(queryAlias.next(), "next")
    assert(match closedQuery.kind { DbErrorKind.Closed => true, _ => false }, "statement alias shares close")
    val invalidText = require(db.prepare("SELECT CAST(x'ff' AS TEXT)"))
    assert(require(invalidText.next()), "read invalid text")
    val invalidUtf8 = failure(invalidText.readText(0), "readText")
    assert(match invalidUtf8.kind { DbErrorKind.Invalid => true, _ => false }, "validate SQLite UTF-8")
    invalidText.close()
    failure(db.prepare("malformed query"), "prepare");
    alias.close()
    db.close()
    val closedDb = failure(db.prepare("SELECT 1"), "prepare")
    assert(match closedDb.kind { DbErrorKind.Closed => true, _ => false }, "connection alias shares close")
    gcCollect()
    assert(missingTable.message() != "", "error details survive close and GC")
    print(text)
}
''', encoding="utf-8")
            for flags in ([], ["--release"]):
                with self.subTest(release=bool(flags)):
                    app = executable(package / "query")
                    built = subprocess.run([COMPILER, "build", package, "-o", app, *flags], capture_output=True, timeout=30)
                    self.assertEqual(built.returncode, 0, built.stderr.decode())
                    result = subprocess.run([app], cwd=package, capture_output=True, timeout=10)
                    self.assertEqual((result.returncode, result.stdout, result.stderr), (0, "李\0Ada\n".encode(), b""))
                    assert_linkage(self, app, 'libsqlite3.so.0')


if __name__ == "__main__":
    unittest.main()
