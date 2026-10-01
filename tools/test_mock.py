"""Run with python3 tools/test_mock.py. No third-party packages."""
import io
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen
import zipfile

from mock_bookorbit import make_server, epub, TOKEN
from fixture_position import resolve


class FixtureTest(unittest.TestCase):
    def test_position_reader_uses_read_only_wal(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"reader.db"
            with sqlite3.connect(path) as writer:
                writer.execute("PRAGMA journal_mode=WAL")
                writer.executescript("CREATE TABLE folders(id INTEGER, name TEXT); CREATE TABLE files(id INTEGER, folder_id INTEGER, filename TEXT, book_id INTEGER); CREATE TABLE books_settings(bookid INTEGER, profileid INTEGER, position TEXT, position_ts INTEGER, cpage INTEGER, npage INTEGER, opentime INTEGER);")
                writer.execute("INSERT INTO folders VALUES(1, '/books')")
                writer.execute("INSERT INTO files VALUES(1, 1, '101.epub', 2)")
                writer.execute("INSERT INTO books_settings VALUES(2, 1, 'known-cfi', 1, 1, 20, 1)")
                writer.commit()
                result = subprocess.run([sys.executable, str(Path(__file__).with_name("inspect_position.py")), str(path), "--filename", "101.epub"],
                                        capture_output=True, text=True, check=True)
                self.assertEqual(json.loads(result.stdout)[0]["position"], "known-cfi")
                with sqlite3.connect(path.resolve().as_uri()+"?mode=ro", uri=True) as reader:
                    with self.assertRaises(sqlite3.OperationalError):
                        reader.execute("DELETE FROM files")

    def test_observed_fixture_positions(self):
        first = resolve(epub(1), "pbr:/webkit?##epubcfi(/6/2!/4/14/1:65)")
        self.assertEqual(first["paragraph"], "p6")
        self.assertTrue(first["after"].startswith("на PocketBook."))
        self.assertEqual(resolve(epub(1), "epubcfi(/6/2!/4/20/1)")["paragraph"], "p9")
        self.assertEqual(resolve(epub(1), "epubcfi(/6/2!/4/26/1)")["paragraph"], "p12")
        for invalid in ("epubcfi(/6/2!/4/3/1)", "epubcfi(/6/2!/4/26/1:9999)",
                        "epubcfi(/6/4!/4/26/1)", "epubcfi(/6/2!/4/26[p12]/1)"):
            with self.assertRaises(ValueError):
                resolve(epub(1), invalid)

    def test_contract_and_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            fault = Path(directory)/"fault"
            server = make_server(port=0, fault_file=fault)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            def request(path, data=None, token=TOKEN):
                req = Request(f"http://127.0.0.1:{server.server_port}/api/v1/{path}",
                              data=json.dumps(data).encode() if data is not None else None,
                              headers={"Content-Type": "application/json", "Authorization": f"Bearer {token}"})
                return urlopen(req, timeout=3)
            try:
                with request("auth/login", {"username": "demo", "password": "demo", "clientKind": "native"}) as response:
                    self.assertEqual(json.load(response)["accessToken"], TOKEN)
                query = {"sort": [], "pagination": {"page": 0, "size": 2}}
                with request("books/query", query) as response:
                    result = json.load(response)
                self.assertEqual(result["total"], 3)
                self.assertEqual(len(result["items"]), 2)
                self.assertIsInstance(result["items"][0]["authors"][0], str)
                with request("books/2") as response:
                    detail = json.load(response)
                self.assertIn(102, [entry["id"] for entry in detail["files"]])
                with request("books/files/101/download") as response:
                    data = response.read()
                self.assertEqual(data, epub(1))
                with zipfile.ZipFile(io.BytesIO(data)) as archive:
                    self.assertEqual(archive.namelist()[0], "mimetype")
                    self.assertEqual(archive.read("mimetype"), b"application/epub+zip")
                    self.assertIn(b'p80', archive.read("chapter.xhtml"))
                with self.assertRaises(HTTPError) as error:
                    request("books/query", query, token="wrong")
                self.assertEqual(error.exception.code, 401)
                fault.write_text("error")
                with self.assertRaises(HTTPError) as error:
                    request("books/query", query)
                self.assertEqual(error.exception.code, 503)
                fault.write_text("malformed")
                with request("books/query", query) as response:
                    with self.assertRaises(ValueError):
                        json.load(response)
            finally:
                server.shutdown()
                server.server_close()
                thread.join()


if __name__ == "__main__":
    unittest.main()
