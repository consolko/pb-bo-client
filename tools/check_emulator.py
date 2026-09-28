"""Integration check on the VM: --pbemu ~/Projects/pbemu --fault-file PATH."""
import argparse
import hashlib
from pathlib import Path
import shlex
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--pbemu", type=Path, required=True)
parser.add_argument("--fault-file", type=Path, required=True)
args = parser.parse_args()
sys.path[:0] = [str(args.pbemu), str(args.pbemu/"tools")]
from pbemu.run import qemu_env_args
from tests.support.runtime import container_sh

data = "/mnt/ext1/system/bookorbit-check"
live = args.pbemu/"U634_6.10.3425/.live"


def run(*options):
    command = ("qemu-arm -L /workspace/firmware/.live/guest " + qemu_env_args() +
               " /mnt/ext1/applications/bookorbit.app --demo --data-dir " + data + " " +
               " ".join(shlex.quote(value) for value in options))
    result = container_sh(command, check=False, timeout=55)
    print(result.stdout, flush=True)
    assert result.returncode == 0, (result.returncode, result.stderr[-1500:])


try:
    args.fault_file.write_text("")
    run("--smoke-test")
    books = list((live/data.lstrip("/")).glob("*/*.epub"))
    assert len(books) == 1, books
    original = hashlib.sha256(books[0].read_bytes()).hexdigest()
    run("--offline-test")
    for mode, operation in [("error", "query"), ("malformed", "query"), ("truncated", "download")]:
        args.fault_file.write_text(mode)
        run("--smoke-test", "--expect-error", operation)
        assert hashlib.sha256(books[0].read_bytes()).hexdigest() == original
        run("--offline-test")
    print("PASS: login, catalog, download, offline restart, HTTP 503, malformed JSON, truncated transfer; cached EPUB preserved.")
finally:
    args.fault_file.write_text("")
