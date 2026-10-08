"""Launch the production client through PATH beside an unrelated same-name file."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    work = root / 'work'
    work.mkdir()
    control = work / binary.name
    control.write_bytes(b'Unrelated file: never replace this with a PocketBook executable.\n')
    original = control.read_bytes()
    data_home = root / 'data'
    data = data_home / 'bookorbit'
    (data / 'update').mkdir(parents=True)
    (data / 'preferences.json').write_text(json.dumps({'diagnosticLogging': True}))
    (data / 'update/preferences.json').write_text(json.dumps({'automatic': False}))
    env = dict(os.environ, PATH=str(binary.parent) + os.pathsep + os.environ.get('PATH', ''),
               XDG_DATA_HOME=str(data_home), QT_QPA_PLATFORM='offscreen', QT_QUICK_BACKEND='software')
    with (root / 'stderr.log').open('wb') as output:
        process = subprocess.Popen([binary.name], cwd=work, env=env, stdout=output, stderr=output)
        try:
            context = None
            deadline = time.monotonic() + 15
            log = data / 'diagnostic.log'
            while time.monotonic() < deadline and process.poll() is None:
                if log.is_file():
                    for line in log.read_text().splitlines():
                        try:
                            entry = json.loads(line)
                        except json.JSONDecodeError:
                            continue  # The process may still be appending this line.
                        if entry.get('event') == 'ota.context':
                            context = entry
                if context:
                    break
                time.sleep(0.05)
            assert context, (root / 'stderr.log').read_text()
            assert context['executable'] == str(binary), context
            assert context['installBlocker'], context
            assert control.read_bytes() == original, 'The same-name control file changed'
            print('PASS: PATH launch resolves the real executable and leaves the control file unchanged')
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
