"""VM regression: ten real QML Read clicks must reuse the stock reader task."""
import argparse
import json
from pathlib import Path
import time
from reader_lab import em, LIVE, OUT, snapshot


def wait_app(suffix):
    until = time.monotonic() + 15
    while time.monotonic() < until:
        info = em.read_task_info(timeout=3)
        if info.appname.endswith(suffix):
            time.sleep(0.6)
            return info
        time.sleep(0.2)
    raise AssertionError('Did not reach ' + suffix)


def run(label):
    baseline_pid = None
    results = []
    for number in range(1, 11):
        offset = (LIVE/'var/log/monitor.log').stat().st_size
        em.run_arm_probe('launch', '--name', 'BookOrbit', '--flags', '0x88', '--',
                         '/mnt/ext1/applications/bookorbit.app')
        wait_app('bookorbit.app')
        # Known U634 portrait demo layout; validate visually before reusing on other firmware.
        em.run_input('touch', '910', '635')
        reader = wait_app('eink-reader.app')
        snapshot(f'{label}-{number:02}-reader')
        if baseline_pid is None:
            baseline_pid = reader.mainpid
        assert reader.mainpid == baseline_pid, (number, baseline_pid, reader.mainpid)
        em.run_input('key', '0x1a')
        wait_app('bookshelf.app')
        snapshot(f'{label}-{number:02}-home')
        with (LIVE/'var/log/monitor.log').open('rb') as log:
            log.seek(offset)
            new = log.read().decode(errors='replace')
        failures = [s for s in ['Segmentation fault', 'core dumped', 'exited with signal'] if s in new]
        assert not failures, (number, failures)
        results.append({'cycle': number, 'reader_pid': reader.mainpid, 'crash_markers': failures})
        print('PASS cycle', number, 'reader PID', reader.mainpid, flush=True)
    (OUT/f'{label}-result.json').write_text(json.dumps(results, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('label')
    args = parser.parse_args()
    if not args.label or any(c not in 'abcdefghijklmnopqrstuvwxyz0123456789-_' for c in args.label):
        parser.error('Use a lowercase alphanumeric label')
    run(args.label)
