#!/usr/bin/env python3
"""Package the same signed ZIP for USB installation and OTA. No network access."""
import argparse
import hashlib
import json
import re
import subprocess
import tempfile
import zipfile
from pathlib import Path


def digest(data):
    return hashlib.sha256(data).hexdigest()


def build_info(binary):
    found = re.findall(rb'BOOKORBIT_BUILD_INFO:(\{[^\x00\r\n]*?\})', binary)
    if len(found) != 1:
        raise ValueError('Expected exactly one build identity in each executable')
    return json.loads(found[0])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--client', type=Path, required=True)
    p.add_argument('--key', type=Path, required=True)
    p.add_argument('--commit', required=True)
    p.add_argument('--source-sha256', help='SHA-256 of the exact source archive used for this build')
    p.add_argument('--expect-version', required=True, help='Reject a binary whose version differs from the release tag')
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--openssl', default='openssl')
    args = p.parse_args()
    client = args.client.read_bytes()
    info = build_info(client)
    public_der = subprocess.check_output([args.openssl, 'pkey', '-in', str(args.key), '-pubout', '-outform', 'DER'])
    if len(public_der) != 44 or public_der[:12] != bytes.fromhex('302a300506032b6570032100'):
        raise ValueError('An Ed25519 signing key is required')
    public = public_der[-32:].hex()
    if info.get('publicKey') != public or info.get('schema') != 3:
        raise ValueError('Signing key or package schema does not match the compiled executable')
    version = info['version']
    if args.expect_version and args.expect_version.removeprefix('v') != version:
        raise ValueError('Release tag and executable version differ')
    if args.source_sha256 and not re.fullmatch('[a-f0-9]{64}', args.source_sha256):
        raise ValueError('Invalid source archive SHA-256')
    if not re.fullmatch(r'(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})', version):
        raise ValueError('A stable numerical version is required')
    if not re.fullmatch('[a-f0-9]{40}', args.commit):
        raise ValueError('A full source commit is required')
    if len(client) < 52 or len(client) > 32*1024*1024 or client[:6] != b'\x7fELF\x01\x01' or int.from_bytes(client[18:20], 'little') != 40 or int.from_bytes(client[36:40], 'little') & 0x400:
        raise ValueError('Expected a bounded ARM ELF32 without hard-float ABI')
    manifest = dict(schema=3, product='bookorbit-pocketbook', version=version, tag='v'+version,
                    commit=args.commit, target='PB634', firmware='U634.6.10.3425', abi='armv7-softfp',
                    layout='single-app', dataFormat=1, bytes=len(client), sha256=digest(client))
    if args.source_sha256:
        manifest['sourceSnapshotSha256'] = args.source_sha256
    raw = json.dumps(manifest, sort_keys=True, separators=(',', ':')).encode()
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        (tmp/'manifest').write_bytes(raw)
        subprocess.run([args.openssl, 'pkeyutl', '-sign', '-rawin', '-inkey', str(args.key),
                        '-in', str(tmp/'manifest'), '-out', str(tmp/'signature')], check=True)
        sig = (tmp/'signature').read_bytes()
        subprocess.run([args.openssl, 'pkey', '-in', str(args.key), '-pubout', '-out', str(tmp/'public')], check=True)
        subprocess.run([args.openssl, 'pkeyutl', '-verify', '-rawin', '-pubin', '-inkey', str(tmp/'public'),
                        '-in', str(tmp/'manifest'), '-sigfile', str(tmp/'signature')], check=True, stdout=subprocess.DEVNULL)
    files = {'bookorbit.app': client, 'release.json': raw, 'release.sig': sig,
             'README.txt': (
                 'BookOrbit PB634 / U634.6.10.3425\n'
                 'USB: close BookOrbit, then copy bookorbit.app into applications/ on internal storage. '
                 'Keep applications/bookorbit/ and your books. Safely eject USB and open BookOrbit.\n'
                 'OTA: Settings > Application updates > Download update > Install and close. '
                 'Open BookOrbit again from the applications menu. No rollback or backup version is kept.\n'
                 'Physical PB634 acceptance is separate from emulator tests.\n'
             ).encode()}
    files['SHA256SUMS'] = ''.join(f'{digest(data)}  {name}\n' for name, data in files.items()).encode()
    args.output.mkdir(parents=True, exist_ok=True)
    archive = args.output/'bookorbit-pb634.zip'
    if archive.exists():
        raise ValueError('Refusing to overwrite an existing release archive')
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in files.items():
            entry = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.external_attr = (0o100755 if name.endswith('.app') else 0o100644) << 16
            z.writestr(entry, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        for name, data in files.items():
            assert z.read(name) == data
    (args.output/'SHA256SUMS').write_text(f'{digest(archive.read_bytes())}  {archive.name}\n')
    if archive.stat().st_size > 32*1024*1024:
        raise ValueError('Release archive exceeds the supported OTA size')
    print(f'Packaged {version}: {archive} ({archive.stat().st_size} bytes)')


if __name__ == '__main__':
    main()
