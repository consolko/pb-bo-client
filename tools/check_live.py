"""Prepared VM only: verify one real EPUB, TLS, native API and ARM Client.

Password is read from the terminal and passed to the ARM process over stdin.
Only login/refresh/logout and library reads are sent to BookOrbit; no progress writes.
"""
import argparse
import getpass
import hashlib
import json
from pathlib import Path
import shlex
import ssl
import socket
import sqlite3
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', required=True)
    parser.add_argument('--username', required=True)
    parser.add_argument('--book-id', required=True, type=int)
    parser.add_argument('--ca', required=True, type=Path)
    parser.add_argument('--data-dir', default='/mnt/ext1/books/BookOrbit-live-check')
    parser.add_argument('--download-dir')
    args = parser.parse_args()
    endpoint = urllib.parse.urlsplit(args.server)
    assert endpoint.scheme == 'https' and endpoint.hostname and not endpoint.username
    assert not endpoint.query and not endpoint.fragment and endpoint.path in ('', '/')
    base = Path.home()/'Projects/pbemu'
    live = base/'U634_6.10.3425/.live'
    sys.path[:0] = [str(base), str(base/'tools')]
    from pbemu.run import qemu_env_args
    password = getpass.getpass('BookOrbit password: ')
    context = ssl.create_default_context(cafile=str(args.ca))
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            return None
    opener = urllib.request.build_opener(urllib.request.HTTPSHandler(context=context), NoRedirect())
    token = ''

    def request(path, payload=None, raw=False):
        headers = {'Content-Type': 'application/json'}
        if token:
            headers['Authorization'] = 'Bearer '+token
        req = urllib.request.Request(args.server.rstrip('/')+'/api/v1/'+path, headers=headers,
                                     data=None if payload is None else json.dumps(payload).encode())
        with opener.open(req, timeout=25) as response:
            return response.read() if raw else json.load(response)

    auth = request('auth/login', dict(username=args.username, password=password,
                                     clientKind='native', deviceLabel='PB634 VM verification'))
    token, refresh = auth['accessToken'], auth['refreshToken']
    report = {'server': args.server, 'bookId': args.book_id, 'loginFields': sorted(auth)}
    try:
        page = request('books/query', {'sort': [], 'pagination': {'page': 0, 'size': 10}, 'q': ''})
        book = next(b for b in page['items'] if b['id'] == args.book_id)
        file = next(f for f in book['files'] if f['format'] == 'epub' and f['role'] == 'primary')
        report.update(total=page['total'], book=book, fileId=file['id'])
        progress_path = f"books/files/{file['id']}/progress"
        report['progressBefore'] = request(progress_path)
        raw = request(f"books/files/{file['id']}/download", raw=True)
        report.update(bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())
        assert len(raw) == file['sizeBytes'] and raw.startswith(b'PK\x03\x04')
        renewed = request('auth/refresh', {'refreshToken': refresh})
        report['refreshRotated'] = renewed['refreshToken'] != refresh
        assert report['refreshRotated']
        token, refresh = renewed['accessToken'], renewed['refreshToken']
        assert request('auth/me')['username'] == args.username

        # The bundled LAN CA must not become trusted for other hosts (including the IP).
        credentials = json.dumps({'username': args.username, 'password': password})+'\n'
        def run(data, *options):
            command = ('exec qemu-arm -L /workspace/firmware/.live/guest '+qemu_env_args()+
                       ' /mnt/ext1/applications/bookorbit.app --data-dir '+shlex.quote(data)+' '+
                       ' '.join(shlex.quote(v) for v in options))
            result = subprocess.run(['podman', 'exec', '-i', 'pb-pocketbook-ui', 'sh', '-c', command],
                                    input=credentials, text=True, capture_output=True, timeout=60)
            print(result.stdout, flush=True)
            if result.returncode:
                print(result.stderr[-3000:], flush=True)
            assert result.returncode == 0, result.returncode
            return result.stdout

        rejected = '/mnt/ext1/system/live-tls-'+uuid.uuid4().hex[:10]
        wrong_host = socket.gethostbyname(endpoint.hostname)
        output = run(rejected, '--server', 'https://'+wrong_host, '--book-id', str(args.book_id),
                     '--smoke-test', '--expect-error', 'auth/login')
        assert 'TLS' in output
        data = args.data_dir
        assert data.startswith('/mnt/ext1/books/') and '..' not in Path(data).parts
        data_root = live/data.lstrip('/')
        data_root.mkdir(parents=True, exist_ok=True)
        assert not (data_root/'certificates'/f'{endpoint.hostname}.pem').exists(), 'Use fresh data to verify embedded CA'
        if args.download_dir:
            assert (live/args.download_dir.lstrip('/')).is_dir()
            (data_root/'preferences.json').write_text(json.dumps({'downloadDirectory': args.download_dir}))
        run(data, '--server', args.server, '--book-id', str(args.book_id), '--smoke-test')
        # No server override: verify restored account in a genuinely new process.
        run(data, '--offline-test')
        scope = hashlib.sha256((args.server.rstrip('/')+'\n'+args.username).encode()).hexdigest()[:24]
        record_dir = live/data.lstrip('/')/scope
        record = json.loads((record_dir/'records'/f"{file['id']}.json").read_text())
        downloaded_dir = record.get('directory', data+'/'+scope)
        downloaded = live/downloaded_dir.lstrip('/')/record['filename']
        assert record['sha256'] == report['sha256'] == hashlib.sha256(downloaded.read_bytes()).hexdigest()
        assert record['bookId'] == args.book_id and record['fileId'] == file['id']
        report['localPath'] = downloaded_dir+'/'+record['filename']
        report['embeddedCA'] = True
        database = live/'mnt/ext1/system/explorer-3/explorer-3.db'
        for attempt in range(20):
            with sqlite3.connect(database.as_uri()+'?mode=ro', uri=True, timeout=.1) as db:
                db.execute('PRAGMA query_only=ON')
                row = db.execute('SELECT book_id FROM files JOIN folders ON folders.id=files.folder_id '
                                 'WHERE folders.name=? AND files.filename=?',
                                 (downloaded_dir, record['filename'])).fetchone()
            if row:
                report['nativeBookId'] = row[0]
                break
            time.sleep(.25)
        else:
            raise AssertionError('Downloaded EPUB was not registered by the native scanner')
        report['progressAfter'] = request(progress_path)
        assert report['progressBefore'] == report['progressAfter']
    finally:
        request('auth/logout', {'refreshToken': refresh})
    try:
        request('auth/refresh', {'refreshToken': refresh})
        raise AssertionError('Revoked refresh token still accepted')
    except urllib.error.HTTPError as error:
        assert error.code == 401
    report['logoutRevoked'] = True
    target = Path.home()/'pbemu-logs'/('real-bookorbit-'+uuid.uuid4().hex[:10]+'.json')
    target.write_text(json.dumps(report, ensure_ascii=False, indent=2))
    print('PASS: verified TLS, native login/refresh/logout, ARM download, SHA-256, native indexing, offline restart; server progress unchanged.')
    print('Report:', target)


if __name__ == '__main__':
    main()
