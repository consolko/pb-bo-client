"""Run a client check with a private HTTP fixture and disposable local state."""
import argparse
import io
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import zipfile

from mock_bookorbit import epub, make_server
from audit_fixtures import write_fixtures


def prepare(directory: Path) -> None:
    with zipfile.ZipFile(io.BytesIO(epub(1))) as original:
        for name, multi in [("compressed.epub", False), ("multi.epub", True)]:
            with zipfile.ZipFile(directory / name, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                for entry in original.namelist():
                    raw = original.read(entry)
                    if multi and entry == "book.opf":
                        raw = raw.replace(b"</manifest>", b'<item id="chapter2" href="chapter2.xhtml" media-type="application/xhtml+xml"/></manifest>')
                        raw = raw.replace(b"</spine>", b'<itemref idref="chapter2" id="second"/></spine>')
                    archive.writestr(entry, raw)
                if multi:
                    archive.writestr("chapter2.xhtml", original.read("chapter.xhtml"))
    write_fixtures(directory)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--native-layout", action="store_true", help="Legacy checks need writable /mnt/ext1 in an isolated container")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    if args.native_layout:
        # Never delete an existing emulated/native storage tree. The legacy
        # checks create uniquely named children and own only those children.
        for name in ("books", "applications"):
            try:
                (Path("/mnt/ext1") / name).mkdir(parents=True, exist_ok=True)
            except PermissionError as error:
                parser.error(f"Use the documented disposable test container: {error}")
    with tempfile.TemporaryDirectory(prefix="bookorbit-check-") as temporary:
        root = Path(temporary)
        fault = root / "fault.txt"
        fault.write_text("")
        prepare(root)
        environment = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software")
        with make_server("127.0.0.1", 0, fault) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            environment["BOOKORBIT_TEST_ENDPOINT"] = f"http://127.0.0.1:{server.server_port}"
            try:
                return subprocess.run([str(binary), str(root), str(fault)], env=environment, timeout=240, check=False).returncode
            finally:
                server.shutdown()
                thread.join(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
