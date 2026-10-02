"""Print persisted U634 reader positions without changing the database."""
import argparse
import json
from pathlib import Path
import sqlite3

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("database", type=Path)
parser.add_argument("--filename", help="Exact basename, e.g. 101.epub")
args = parser.parse_args()
with sqlite3.connect(args.database.resolve().as_uri() + "?mode=ro", uri=True, timeout=2) as db:
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA query_only=ON")
    rows = db.execute("""
        SELECT folders.name AS folder, files.filename, files.book_id,
               books_settings.profileid, books_settings.position,
               books_settings.position_ts, books_settings.cpage,
               books_settings.npage, books_settings.opentime
        FROM files JOIN folders ON folders.id=files.folder_id
        LEFT JOIN books_settings ON books_settings.bookid=files.book_id
        WHERE (? IS NULL OR files.filename=?)
        ORDER BY files.id, books_settings.profileid
    """, (args.filename, args.filename)).fetchall()
print(json.dumps([dict(row) for row in rows], ensure_ascii=False, indent=2))
