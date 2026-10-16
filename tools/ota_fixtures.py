"""Build disposable signed OTA fixtures and a test-only key header; no release key is used."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import zipfile

folder=Path(sys.argv[1]); folder.mkdir(parents=True,exist_ok=True)
version=sys.argv[2]; openssl=sys.argv[3]
key=folder/'key.pem'
if not key.exists():
    subprocess.run([openssl,'genpkey','-algorithm','Ed25519','-out',str(key)],check=True)
public=subprocess.check_output([openssl,'pkey','-in',str(key),'-pubout','-outform','DER'])[-32:].hex()
header='#undef BOOKORBIT_UPDATE_PUBLIC_KEY\n#define BOOKORBIT_UPDATE_PUBLIC_KEY "'+public+'"\n'
if not (folder/'key.h').exists() or (folder/'key.h').read_text()!=header: (folder/'key.h').write_text(header)
major,minor,patch=map(int,version.split('.'))
assert 0<major<999999 and patch<999998, 'Test version needs room for older and newer fixtures'
for name,number in [('current',version),('next',f'{major}.{minor}.{patch+1}'),('newest',f'{major}.{minor}.{patch+2}'),('old',f'{major-1}.{minor}.{patch}')]:
    binary=bytearray(52); binary[:6]=b'\x7fELF\x01\x01'; binary[18]=40
    binary.extend(number.encode())
    manifest=dict(schema=3,product='bookorbit-pocketbook',version=number,tag='v'+number,target='PB634',firmware='U634.6.10.3425',abi='armv7-softfp',layout='single-app',dataFormat=1,sha256=hashlib.sha256(binary).hexdigest(),bytes=len(binary))
    raw=json.dumps(manifest,separators=(',',':'),sort_keys=True).encode()
    source=folder/'manifest'; signature=folder/'signature'
    source.write_bytes(raw)
    subprocess.run([openssl,'pkeyutl','-sign','-rawin','-inkey',str(key),'-in',str(source),'-out',str(signature)],check=True)
    with zipfile.ZipFile(folder/(name+'.zip'),'w') as archive:
        for file,data in [('bookorbit.app',binary),('release.json',raw),('release.sig',signature.read_bytes())]: archive.writestr(file,data)
