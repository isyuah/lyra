import argparse
from pathlib import Path
import sqlite3
import sys


root = Path(__file__).resolve().parents[1]


def snapshot():
    schema = (root / 'inputs/schema.sql').read_bytes().decode('utf-8')
    query = (root / 'tools/userById.sql').read_text(encoding='utf-8').strip()
    db = sqlite3.connect(':memory:')
    try:
        db.executescript(schema)
        columns = {row[1]: row for row in db.execute('PRAGMA table_info(users)')}
        if 'id' not in columns or columns['id'][2].upper() != 'INTEGER' or not columns['id'][5]:
            raise ValueError('users.id must be an INTEGER PRIMARY KEY')
        if 'name' not in columns or columns['name'][2].upper() != 'TEXT' or not columns['name'][3]:
            raise ValueError('users.name must be TEXT NOT NULL')
        db.execute('PRAGMA query_only=ON')
        try:
            db.execute('EXPLAIN ' + query)
        except sqlite3.ProgrammingError:
            db.execute('EXPLAIN ' + query, {'1': 0})
        else:
            raise ValueError('expected the ?1 user id parameter')
        result = db.execute('SELECT * FROM (' + query + ') LIMIT 0', {'1': 0})
        if [column[0] for column in result.description] != ['name']:
            raise ValueError('expected one result column named name')
    finally:
        db.close()
    return 'lyra-sqlite-example/1\nparameter: Int; column: name String\n' + query + '\n-- schema --\n' + schema


def main():
    parser = argparse.ArgumentParser(description='Validate the example query against its schema in an empty in-memory SQLite database.')
    parser.add_argument('--check', action='store_true', help='compare with the stored snapshot without writing')
    args = parser.parse_args()
    path = root / 'inputs/userById.snapshot'
    try:
        expected = snapshot().encode('utf-8')
        if args.check:
            if not path.is_file() or path.read_bytes() != expected:
                raise ValueError('stale userById.snapshot; run tools/prepareQuery.py')
        else:
            path.write_bytes(expected)
    except (OSError, ValueError, sqlite3.Error) as error:
        print('userById: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
