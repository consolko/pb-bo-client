"""VM-only observations on the prepared U634 lab; no native database writes."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

BASE = Path.home() / 'Projects/pbemu'
LIVE = BASE / 'U634_6.10.3425/.live'
OUT = Path.home() / 'pbemu-logs/stages-1-2'
sys.path[:0] = [str(BASE), str(BASE / 'tools')]
from tests.support.runtime import Emulator

em = Emulator(firmware='U634_6.10.3425')


def snapshot(label):
    if not label or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789-_' for c in label):
        raise ValueError('Use a lowercase alphanumeric label')
    target = OUT / label
    target.mkdir(parents=True, exist_ok=False)
    (target/'time.json').write_text(json.dumps({'utc': datetime.now(timezone.utc).isoformat(), 'monotonic': time.monotonic()}))
    for name in ['monitor.log', 'viewer.log', 'informer.log']:
        path = LIVE/'var/log'/name
        if path.exists():
            shutil.copyfile(path, target/name)
    processes = subprocess.run(['podman', 'exec', 'pb-pocketbook-ui', 'ps', '-eo', 'pid,ppid,stat,comm,args'], capture_output=True, text=True, check=True)
    (target/'processes.txt').write_text(processes.stdout)
    (target/'task.txt').write_text(em.run_arm_probe('task-info').stdout)
    position = subprocess.run([sys.executable, str(Path(__file__).with_name('inspect_position.py')),
                              str(LIVE/'mnt/ext1/system/explorer-3/explorer-3.db'), '--filename', '101.epub'],
                             capture_output=True, text=True, check=True)
    (target/'position.json').write_text(position.stdout)
    (target/'frame.txt').write_text(em.run_probe('frame_dump').stdout)
    pid = subprocess.check_output(['podman', 'inspect', '--format', '{{.State.Pid}}', 'pb-pocketbook-ui'], text=True).strip()
    with (target/'frame.pgm').open('wb') as output:
        subprocess.run(['podman', 'unshare', 'nsenter', '--target', pid, '--ipc', sys.executable,
                        str(Path(__file__).with_name('capture_frame.py')), str(target/'frame.txt')], stdout=output, check=True)
    print(target, flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('label')
    snapshot(parser.parse_args().label)
