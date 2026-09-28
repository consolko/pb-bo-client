"""Local BookOrbit contract fixture; synthetic books, no production credentials."""
import argparse
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from pathlib import Path
import re
import struct
import time
import zlib
import zipfile

TOKEN = "local-development-only"


def epub(number):
    """Deterministic EPUB with identifiable paragraphs for position experiments."""
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as archive:
        def put(name, content):
            archive.writestr(zipfile.ZipInfo(name, (2026, 9, 28, 0, 0, 0)), content)
        put("mimetype", "application/epub+zip")
        put("META-INF/container.xml", '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="book.opf" media-type="application/oebps-package+xml"/></rootfiles></container>')
        put("book.opf", f'''<?xml version="1.0"?><package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">urn:pb-bo:test:{number}</dc:identifier><dc:title>Тестовая книга {number}</dc:title><dc:language>ru</dc:language><dc:creator>Локальный стенд</dc:creator><meta property="dcterms:modified">2026-09-28T00:00:00Z</meta></metadata><manifest><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>''')
        put("nav.xhtml", '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Оглавление</title></head><body><nav epub:type="toc"><ol><li><a href="chapter.xhtml">Проверка позиции</a></li></ol></nav></body></html>')
        paragraphs = "".join(f'<p id="p{i}">Книга {number}, абзац {i:03d}. Это контрольный фрагмент для проверки чтения на PocketBook. Запомните номер абзаца перед закрытием книги и сравните его после повторного открытия.</p>' for i in range(1, 81))
        put("chapter.xhtml", f'<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Тестовая книга {number}</title></head><body><h1>Тестовая книга {number}</h1>{paragraphs}</body></html>')
    return out.getvalue()


def cover(number):
    """Small deterministic PNG cover; no image library needed by the fixture."""
    w, h = 160, 240
    def chunk(kind, data):
        return struct.pack(">I", len(data))+kind+data+struct.pack(">I", zlib.crc32(kind+data))
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        for x in range(w):
            border = x < 10 or x >= w-10 or y < 10 or y >= h-10
            stripe = 65 < y < 80 or 90 < y < 99 or (x//20+y//20+number) % 7 == 0
            rows.extend((35, 35, 35) if border else ((100+number*30, 90, 80) if stripe else (230, 222, 200)))
    return b"\x89PNG\r\n\x1a\n"+chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))+chunk(b"IDAT", zlib.compress(bytes(rows)))+chunk(b"IEND", b"")


BOOKS = [{"id": i, "title": f"Тестовая книга {i}", "authors": ["Локальный стенд"],
          "hasCover": i != 3, "coverVersion": "1",
          "files": [{"id": 100+i, "format": "epub", "role": "primary", "sizeBytes": len(epub(i))}]}
         for i in range(1, 4)]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        # Never log bodies, passwords or Authorization headers.
        print(fmt % args, flush=True)

    def send(self, code, data, content_type="application/json"):
        raw = json.dumps(data, ensure_ascii=False).encode() if content_type == "application/json" else data
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        try:
            self.wfile.write(raw)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def route(self, body):
        path = self.path.split("?", 1)[0]
        if path == "/api/v1/auth/login" and self.command == "POST":
            if (body.get("username"), body.get("password"), body.get("clientKind")) != ("demo", "demo", "native"):
                return self.send(401, {"message": "Use demo / demo on this local fixture"})
            expiry = (datetime.now(timezone.utc)+timedelta(hours=1)).isoformat()
            return self.send(200, {"accessToken": TOKEN, "refreshToken": "0"*64, "accessTokenExpiresAt": expiry, "refreshTokenExpiresAt": expiry, "user": {"id": 1, "username": "demo"}})
        if self.headers.get("Authorization") != f"Bearer {TOKEN}":
            return self.send(401, {"message": "Unauthorized"})
        fault = self.server.fault_file
        mode = fault.read_text().strip() if fault and fault.exists() else ""
        if mode == "expired":
            return self.send(401, {"message": "Expired session"})
        thumbnail = re.fullmatch(r"/api/v1/books/([1-3])/thumbnail", path)
        if thumbnail and self.command == "GET":
            if thumbnail[1] == "3" or (mode == "cover_removed" and thumbnail[1] == "1"):
                return self.send(404, {"message": "No cover"})
            return self.send(200, cover(4 if mode == "cover_changed" and thumbnail[1] == "1" else int(thumbnail[1])), "image/png")
        if mode == "error":
            return self.send(503, {"message": "Simulated outage"})
        if path == "/api/v1/books/query" and self.command == "POST":
            if mode == "malformed":
                return self.send(200, b'{"items":', "text/plain")
            page, size = body.get("pagination", {}).get("page"), body.get("pagination", {}).get("size")
            if type(page) is not int or page < 0 or type(size) is not int or not 1 <= size <= 100:
                return self.send(400, {"message": "Invalid pagination"})
            query = str(body.get("q", "")).casefold()
            items = [dict(book) for book in BOOKS if query in book["title"].casefold()]
            if mode == "cover_removed":
                for book in items:
                    if book["id"] == 1:
                        book["hasCover"] = False
            if mode == "cover_changed":
                for book in items:
                    if book["id"] == 1:
                        book["coverVersion"] = "2"
            return self.send(200, {"items": items[page*size:(page+1)*size], "total": len(items), "page": page, "size": size})
        match = re.fullmatch(r"/api/v1/books/files/(10[1-3])/(download|progress)", path)
        if match and self.command == "GET":
            if match[2] == "progress":
                return self.send(200, {"cfi": None, "pageNumber": None, "percentage": 0, "textUpdatedAt": None})
            raw = epub(9 if mode == "changed" and match[1] == "101" else int(match[1])-100)
            if mode == "slow":
                self.send_response(200)
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                try:
                    for offset in range(0, len(raw), 256):
                        self.wfile.write(raw[offset:offset+256])
                        self.wfile.flush()
                        time.sleep(0.05)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                return
            if mode == "truncated":
                self.send_response(200)
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw[:100])
                self.close_connection = True
                return
            return self.send(200, raw, "application/epub+zip")
        return self.send(404, {"message": "Route not implemented by local fixture"})

    def do_GET(self):
        self.route({})

    def do_POST(self):
        try:
            size = int(self.headers.get("Content-Length", "0"))
            if not 0 < size <= 65536:
                raise ValueError("Invalid size")
            body = json.loads(self.rfile.read(size))
            if not isinstance(body, dict):
                raise ValueError("Expected object")
        except (ValueError, UnicodeError):
            return self.send(400, {"message": "Invalid JSON body"})
        self.route(body)


def make_server(host="127.0.0.1", port=8765, fault_file=None):
    server = ThreadingHTTPServer((host, port), Handler)
    server.fault_file = fault_file
    return server


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--fault-file", type=Path)
    args = parser.parse_args()
    with make_server(args.host, args.port, args.fault_file) as server:
        print(f"LOCAL FIXTURE http://{args.host}:{server.server_port}; demo / demo", flush=True)
        server.serve_forever()
