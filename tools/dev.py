"""Mac entry point for the prepared VM: build, deploy, start, snapshot."""
import argparse
from datetime import datetime, timezone
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
VM = "consolko@192.168.64.5"
IMAGE = "ghcr.io/fstanis/pocketbook-sdk-qt6-builder@sha256:4028eba9874caf760592e9d3e81b8f396ef78413c74758a4608fa0a4a8eff62a"


def remote(script, **kwargs):
    return subprocess.run(["ssh", VM, "sh", "-s"], input="set -eu\n" + script,
                          text=True, check=True, **kwargs)


def snapshot():
    target = ROOT / "logs" / ("snapshot-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ"))
    target.mkdir(parents=True)
    with (target / "firmware-logs.tar").open("wb") as out:
        subprocess.run(["ssh", VM, "tar -C ~/Projects/pbemu/U634_6.10.3425/.live/var/log -cf - ."],
                       stdout=out, check=True)
    result = remote('podman ps; if podman container exists pb-pocketbook-ui; then podman exec pb-pocketbook-ui ps -eo pid,ppid,stat,comm,args; fi', capture_output=True)
    (target / "processes.txt").write_text(result.stdout)
    print(target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["build", "deploy", "start", "snapshot"])
    args = parser.parse_args()
    if args.action == "snapshot":
        snapshot()
    elif args.action == "build":
        source = subprocess.Popen(["tar", "-C", str(ROOT / "pb-bo-client"), "--exclude=.git",
                                   "--exclude=__pycache__", "--exclude=build", "-cf", "-", "."], stdout=subprocess.PIPE)
        try:
            subprocess.run(["ssh", VM, "tar -xf - -C ~/Projects/pb-bo-workspace/pb-bo-client"],
                           stdin=source.stdout, check=True)
        finally:
            source.stdout.close()
            code = source.wait()
        if code:
            raise subprocess.CalledProcessError(code, source.args)
        remote(f'''podman run --rm --userns=keep-id -v "$HOME/Projects/pb-bo-workspace:/work" \
          -w /work/pb-bo-client {IMAGE} bash -lc \
          'cmake -S . -B /work/build -DPOCKETBOOK_QT_SDK=/work/sdk-qt6 -DCMAKE_BUILD_TYPE=Release && cmake --build /work/build -j2'
          sha256sum "$HOME/Projects/pb-bo-workspace/build/bookorbit.app"''')
    elif args.action == "deploy":
        snapshot()
        remote('''cd "$HOME/Projects/pbemu"
          test -x "$HOME/Projects/pb-bo-workspace/build/bookorbit.app"
          ./pbemu stop
          cp "$HOME/Projects/pb-bo-workspace/build/bookorbit.app" U634_6.10.3425/.live/mnt/ext1/applications/bookorbit.app
          XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 ./pbemu start --no-audio U634_6.10.3425''')
    else:
        remote('''cd "$HOME/Projects/pbemu"
.venv/bin/python - <<'PY'
from tests.support.runtime import Emulator
print(Emulator(firmware="U634_6.10.3425").run_arm_probe(
    "launch", "--name", "BookOrbit", "--flags", "0x88", "--",
    "/mnt/ext1/applications/bookorbit.app").stdout)
PY''')


if __name__ == "__main__":
    main()
