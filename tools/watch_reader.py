"""Preserve monitor output across pbemu test restarts, plus periodic process lists."""
import argparse
import json
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--seconds', type=int, default=60, choices=range(1, 121), metavar='1..120')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
log = Path.home()/'Projects/pbemu/U634_6.10.3425/.live/var/log/monitor.log'
previous = None
position = 0
last_ps = 0
until = time.monotonic() + args.seconds
with (args.output/'monitor-history.log').open('wb') as history, (args.output/'processes.jsonl').open('w') as processes:
    while time.monotonic() < until:
        try:
            stat = log.stat()
            if stat.st_ino != previous or stat.st_size < position:
                position = 0
                previous = stat.st_ino
                history.write(f'\n--- LOG START {time.time()} ---\n'.encode())
            with log.open('rb') as source:
                source.seek(position)
                chunk = source.read()
                position += len(chunk)
            history.write(chunk)
            history.flush()
        except FileNotFoundError:
            previous = None
        if time.monotonic() - last_ps >= 1:
            r = subprocess.run(['podman', 'exec', 'pb-pocketbook-ui', 'ps', '-eo', 'pid,ppid,stat,comm,args'],
                               capture_output=True, text=True, timeout=5)
            processes.write(json.dumps({'time': time.time(), 'returncode': r.returncode, 'ps': r.stdout}) + '\n')
            processes.flush()
            last_ps = time.monotonic()
        time.sleep(0.05)
