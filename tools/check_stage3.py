"""VM: run optional client_check.app against an isolated fixture and fresh data."""
from pathlib import Path
import shutil
import sys
import threading
import uuid
from mock_bookorbit import make_server

base = Path.home()/'Projects/pbemu'
live = base/'U634_6.10.3425/.live'
sys.path[:0] = [str(base), str(base/'tools')]
from tests.support.runtime import container_sh
from pbemu.run import qemu_env_args

name = 'stage3-check-'+uuid.uuid4().hex[:10]
data = '/mnt/ext1/system/'+name
folder = live/data.lstrip('/')
folder.mkdir()
fault = folder/'fault.txt'
fault.write_text('')
source = Path.home()/'Projects/pb-bo-workspace/build/client_check.app'
shutil.copy2(source, live/'mnt/ext1/applications/client_check.app')
with make_server('0.0.0.0', 8766, fault) as server:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        command = ('qemu-arm -L /workspace/firmware/.live/guest '+qemu_env_args()+
                   ' /mnt/ext1/applications/client_check.app '+data+' '+data+'/fault.txt')
        result = container_sh(command, check=False, timeout=90)
        print(result.stdout)
        print(result.stderr)
        print('Artifacts:', folder)
        if result.returncode:
            raise SystemExit(result.returncode)
    finally:
        server.shutdown()
        thread.join()
