from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from platform_cli import assert_linkage, executable


COMPILER = Path(sys.argv.pop(1)).resolve()


class SQLite(unittest.TestCase):
    def test_raw_handle_constructors_are_private(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary)
            (package / 'src').mkdir()
            (package / 'lyra.json5').write_text("{name:'test/sqlite-handle',version:'1.0.0'}", encoding='utf-8')
            for name in ('Connection', 'Statement', 'Transaction'):
                with self.subTest(name=name):
                    (package / 'src/main.ly').write_text(
                        f'import std.sqlite.{{{name}}}\nfn main(): Unit {{ val _ = {name}(CPtr.nullPtr()) }}\n', encoding='utf-8')
                    result = subprocess.run([COMPILER, 'check', package], capture_output=True, timeout=30)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(b'LYN0003', result.stderr)

    def test_prepared_statement_cleanup_and_copied_results(self):
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary)
            (package / "src").mkdir()
            (package / "lyra.json5").write_text("{name:'test/query',version:'1.0.0'}")
            (package / "src" / "main.ly").write_text(r'''
import std.sqlite.{Busy}
import std.sqlite.{Connection}
import std.sqlite.{DbError}
import std.sqlite.{DbErrorKind}
import std.sqlite.{open}
import std.sqlite.{openReadOnly}

fn require<T>(result: Result<T, DbError>): T {
    match result { Ok(value) => value, Err(error) => throw Exception(error.message()) }
}

fn failure<T>(result: Result<T, DbError>, operation: String): DbError {
    match result {
        Ok(_) => throw Exception("expected SQLite failure"),
        Err(error) => {
            #assert(error.operation == operation && error.code != 0 && error.message() != "", "missing error details")
            error
        },
    }
}

fn insertPerson(db: Connection): Result<Unit, DbError> {
    val insert = db.prepare("INSERT INTO people VALUES(?1,?2)")?
    defer insert.close()
    insert.bindInt(1, 7)?
    insert.bindText(2, "李\0Ada")?
    #assert(!insert.next()?, "insert unexpectedly returned a row")
    Ok({})
}

fn main(): Unit {
    val missingFile = failure(openReadOnly("does-not-exist.db"), "open")
    #assert(match missingFile.kind { DbErrorKind.Sqlite => true, _ => false }, "missing file")
    val db = require(open(":memory:"))
    val alias = db
    val missingTable = failure(db.prepare("SELECT * FROM missing"), "prepare")
    #assert(match missingTable.kind { DbErrorKind.Sqlite => true, _ => false }, "missing table")
    val create = require(db.prepare("CREATE TABLE people(id INTEGER PRIMARY KEY, name TEXT)"))
    #assert(!require(create.next()), "table")
    create.close()
    require(insertPerson(db))
    val duplicate = failure(insertPerson(db), "next")
    #assert(match duplicate.kind { DbErrorKind.Sqlite => true, _ => false }, "step error stayed a Result after defer")
    val missingRow = require(db.prepare("SELECT name FROM people WHERE id=8"))
    #assert(!require(missingRow.next()) && !require(missingRow.next()), "no row is success")
    missingRow.close()
    val query = require(db.prepare("SELECT name, id, NULL, '' FROM people WHERE id=?1"))
    val queryAlias = query
    require(query.bindInt(1, 7))
    #assert(require(query.next()), "row")
    val text = match require(query.readText(0)) { Some(value) => value, None => throw Exception("missing text") }
    #assert(match require(query.readInt(1)) { Some(7) => true, _ => false }, "typed reads")
    #assert(require(query.readText(2)) == null && require(query.readInt(2)) == null, "SQL NULL is empty Option")
    #assert(match require(query.readText(3)) { Some("") => true, _ => false }, "empty text is not SQL NULL")
    val wrongType = failure(query.readInt(0), "readInt")
    #assert(match wrongType.kind { DbErrorKind.Invalid => true, _ => false }, "invalid storage type")
    failure(query.readInt(4294967296), "readInt");
    val busy = try { alias.close(); false } catch error: Busy { error.message != "" }
    #assert(busy && !require(query.next()), "busy close kept connection open for statement")
    query.close()
    queryAlias.close()
    val closedQuery = failure(queryAlias.next(), "next")
    #assert(match closedQuery.kind { DbErrorKind.Closed => true, _ => false }, "statement alias shares close")
    val invalidText = require(db.prepare("SELECT CAST(x'ff' AS TEXT)"))
    #assert(require(invalidText.next()), "read invalid text")
    val invalidUtf8 = failure(invalidText.readText(0), "readText")
    #assert(match invalidUtf8.kind { DbErrorKind.Invalid => true, _ => false }, "validate SQLite UTF-8")
    invalidText.close()
    failure(db.prepare("malformed query"), "prepare");
    alias.close()
    db.close()
    val closedDb = failure(db.prepare("SELECT 1"), "prepare")
    #assert(match closedDb.kind { DbErrorKind.Closed => true, _ => false }, "connection alias shares close")
    gcCollect()
    #assert(missingTable.message() != "", "error details survive close and GC")
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
                    assert_linkage(self, app, 'libm.so.6')

    def test_scoped_transactions_and_storage_types(self):
        source = r'''
import std.sqlite.{Busy}
import std.sqlite.{Connection}
import std.sqlite.{DbError}
import std.sqlite.{DbErrorKind}
import std.sqlite.{Statement}
import std.sqlite.{Transaction}
import std.sqlite.{open}

exception Stop
struct Escaped(pub val transaction: Transaction, pub val cursor: Statement, pub val rowid: Int)

fn require<T>(result: Result<T, DbError>): T {
    match result { Ok(value) => value, Err(error) => throw Exception(error.operation + ": " + error.message()) }
}

fn expecting<T>(result: Result<T, DbError>, kind: DbErrorKind, operation: String): DbError {
    match result {
        Ok(_) => throw Exception("expected failure: " + operation),
        Err(error) => {
            #assert(error.kind == kind && error.operation == operation && error.code != 0 && error.message() != "")
            error
        },
    }
}

fn count(db: Connection, sql: String): Int {
    val rows = require(db.prepare(sql))
    defer rows.close()
    #assert(require(rows.next()))
    require(rows.readInt(0)) ?? -1
}

fn insert(tx: Transaction): Result<Int, DbError> {
    val statement = tx.prepare("INSERT INTO data(real, blob, empty, absent) VALUES(?1, ?2, ?3, ?4)")?
    defer statement.close()
    expecting(statement.bindInt(0, 7), DbErrorKind.Invalid, "bindInt");
    expecting(statement.bindNull(5), DbErrorKind.Invalid, "bindNull");
    expecting(statement.bindText(4294967297, "not a bounded index"), DbErrorKind.Invalid, "bindText");
    val zero: Float64 = 0
    expecting(statement.bindFloat(1, zero / zero), DbErrorKind.Invalid, "bindFloat");
    statement.bindFloat(1, 0.125)?
    statement.bindBlob(2, b"\xff\0A")?
    statement.bindBlob(3, b"")?
    statement.bindNull(4)?
    #assert(!statement.next()?)
    tx.lastInsertRowid()
}

fn retryAfterLock(): Unit {
    val first = require(open("contention.db"))
    defer first.close()
    val second = require(open("contention.db"))
    defer second.close()
    require(first.exec("CREATE TABLE IF NOT EXISTS writes(value INTEGER)"))
    require(first.exec("DELETE FROM writes"))
    require(first.transaction<Unit>() { tx =>
        tx.exec("INSERT INTO writes VALUES(1)")?
        expecting(second.exec("INSERT INTO writes VALUES(2)"), DbErrorKind.Busy, "exec");
        Ok({})
    })
    require(second.exec("INSERT INTO writes VALUES(2)"))
    #assert(count(second, "SELECT count(*) FROM writes") == 2)
}

fn main(): Unit {
    val db = require(open(":memory:"))
    defer db.close()
    val alias = db
    require(db.exec("CREATE TABLE data(id INTEGER PRIMARY KEY, real REAL, blob BLOB, empty BLOB, absent REAL)"))
    #assert(require(db.lastInsertRowid()) == 0)
    val openCursor = require(db.prepare("SELECT 1"))
    expecting(db.transaction<Unit>() { tx => throw Exception("body must not run") }, DbErrorKind.Busy, "begin");
    openCursor.close()

    val escaped = require(db.transaction<Escaped>() { tx =>
        expecting(alias.exec("INSERT INTO data(id) VALUES(100)"), DbErrorKind.Busy, "exec");
        expecting(alias.transaction<Unit>() { inner => throw Exception("nested body must not run") }, DbErrorKind.Busy, "begin");
        #assert(try { alias.close(); false } catch _: Busy { true })
        expecting(tx.exec("/* cannot escape */ END"), DbErrorKind.Invalid, "exec");
        expecting(tx.prepare("SAVEPOINT nested"), DbErrorKind.Invalid, "prepare");
        val rowid = insert(tx)?
        val prepared = tx.prepare("SELECT 1")?
        Ok(Escaped(tx, prepared, rowid))
    })
    expecting(escaped.transaction.lastInsertRowid(), DbErrorKind.Closed, "lastInsertRowid");
    expecting(escaped.cursor.next(), DbErrorKind.Closed, "next");
    expecting(escaped.cursor.bindNull(0), DbErrorKind.Closed, "bindNull");
    expecting(escaped.cursor.readInt(4294967296), DbErrorKind.Closed, "readInt");
    escaped.cursor.close()
    #assert(escaped.rowid == 1 && require(db.lastInsertRowid()) == 1)

    val query = require(db.prepare("SELECT real, blob, empty, absent, id FROM data"))
    #assert(require(query.next()))
    #assert(require(query.readFloat(0)) == Some(0.125) && require(query.readFloat(3)) == None)
    #assert(require(query.readBlob(3)) == None && require(query.readBlob(2)) == Some(b""))
    expecting(query.readFloat(4), DbErrorKind.Invalid, "readFloat");
    expecting(query.readBlob(0), DbErrorKind.Invalid, "readBlob");
    val blob = require(query.readBlob(1))
    query.close()
    gcCollect()
    #assert(blob == Some(b"\xff\0A"))

    for sql in ["SELECT 1", "INSERT INTO data(id) VALUES(?1)", "INSERT INTO data(id) VALUES(2) RETURNING id",
                "INSERT INTO data(id) VALUES(3); INSERT INTO data(id) VALUES(4)", "SELECT 1\0; SELECT 2"] {
        expecting(db.exec(sql), DbErrorKind.Invalid, "exec");
    }
    #assert(count(db, "SELECT count(*) FROM data") == 1)

    val primary = expecting(db.transaction<Unit>() { tx =>
        tx.exec("INSERT INTO data(id) VALUES(2)")?
        tx.exec("INSERT INTO missing_table VALUES(1)")
    }, DbErrorKind.Sqlite, "exec")
    #assert(primary.message() != "" && primary.cleanupError == None)
    #assert(try {
        db.transaction<Unit>() { tx =>
            tx.exec("INSERT INTO data(id) VALUES(3)")?
            throw Stop("keep original")
        };
        false
    } catch original: Stop { original.message == "keep original" })
    #assert(count(db, "SELECT count(*) FROM data") == 1)

    expecting(db.transaction<Unit>() { tx =>
        tx.exec("INSERT INTO data(id) VALUES(4)")?
        expecting(tx.exec("INSERT OR ROLLBACK INTO data(id) VALUES(1)"), DbErrorKind.Sqlite, "exec");
        expecting(tx.exec("INSERT INTO data(id) VALUES(5)"), DbErrorKind.Closed, "exec");
        Ok({})
    }, DbErrorKind.Sqlite, "commit");
    #assert(count(db, "SELECT count(*) FROM data") == 1)

    require(db.exec("PRAGMA foreign_keys = ON"))
    require(db.exec("CREATE TABLE parent(id INTEGER PRIMARY KEY)"))
    require(db.exec("CREATE TABLE child(parent REFERENCES parent(id) DEFERRABLE INITIALLY DEFERRED)"))
    expecting(db.transaction<Unit>() { tx => tx.exec("INSERT INTO child VALUES(7)") }, DbErrorKind.Sqlite, "commit");
    #assert(count(db, "SELECT count(*) FROM child") == 0)
    require(db.transaction<Unit>() { tx =>
        tx.exec("INSERT INTO parent VALUES(7)")?
        tx.exec("INSERT INTO child VALUES(7)")
    })
    #assert(count(db, "SELECT count(*) FROM child") == 1)
    retryAfterLock()
    print("transaction ok")
}
'''
        with tempfile.TemporaryDirectory() as temporary:
            package = Path(temporary)
            (package / 'src').mkdir()
            (package / 'lyra.json5').write_text("{name:'test/transaction',version:'1.0.0'}", encoding='utf-8')
            (package / 'src/main.ly').write_text(source, encoding='utf-8')
            for flags in ([], ['--release']):
                with self.subTest(flags=flags):
                    run = subprocess.run([COMPILER, 'run', package, *flags], cwd=package, capture_output=True, timeout=60)
                    self.assertEqual(run.returncode, 0, run.stderr.decode())
                    self.assertEqual(run.stdout.decode().splitlines(), ['transaction ok'])


if __name__ == "__main__":
    unittest.main()
