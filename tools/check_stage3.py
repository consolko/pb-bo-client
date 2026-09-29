"""VM: run optional client_check.app against an isolated fixture and fresh data."""
from pathlib import Path
import shutil
import io
import zipfile
import sys
import threading
import uuid
from mock_bookorbit import make_server, epub

base = Path.home()/'Projects/pbemu'
live = base/'U634_6.10.3425/.live'
sys.path[:0] = [str(base), str(base/'tools')]
from tests.support.runtime import container_sh
from pbemu.run import qemu_env_args

name = 'stage3-check-'+uuid.uuid4().hex[:10]
data = '/mnt/ext1/system/'+name
folder = live/data.lstrip('/')
folder.mkdir()
print('Artifacts:', folder, flush=True)
fault = folder/'fault.txt'
fault.write_text('')
with zipfile.ZipFile(io.BytesIO(epub(1))) as original:
    for name, multi in [("compressed.epub", False), ("multi.epub", True)]:
        with zipfile.ZipFile(folder/name, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for entry in original.namelist():
                epub_bytes = original.read(entry)
                if multi and entry == "book.opf":
                    epub_bytes = epub_bytes.replace(b"</manifest>", b'<item id="chapter2" href="chapter2.xhtml" media-type="application/xhtml+xml"/></manifest>').replace(b"</spine>", b'<itemref idref="chapter2" id="second"/></spine>')
                archive.writestr(entry, epub_bytes)
            if multi:
                archive.writestr("chapter2.xhtml", original.read("chapter.xhtml"))
binary = 'catalog_ui_check.app' if '--catalog-ui' in sys.argv else 'native_sync_check.app' if '--native' in sys.argv else 'client_check.app'
source = Path.home()/'Projects/pb-bo-workspace/build'/binary
shutil.copy2(source, live/'mnt/ext1/applications'/binary)
with make_server('0.0.0.0', 8766, fault) as server:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        command = ('qemu-arm -L /workspace/firmware/.live/guest '+qemu_env_args()+
                   ' /mnt/ext1/applications/'+binary+' '+data+' '+data+'/fault.txt')
        if binary in ('native_sync_check.app', 'catalog_ui_check.app') and '--ui' in sys.argv:
            command += ' --ui'
        result = container_sh(command, check=False, timeout=300 if '--ui' in sys.argv else 120)
        print(result.stdout)
        print(result.stderr)
        print('Artifacts:', folder)
        if result.returncode:
            raise SystemExit(result.returncode)
    finally:
        server.shutdown()
        thread.join()
