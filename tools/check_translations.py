"""Check catalog coverage and placeholders without requiring Qt build tools."""
from pathlib import Path
import collections
import json
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
catalog = ET.parse(root / 'translations/bookorbit_ru.ts')
messages = {m.findtext('source'): m.findtext('translation') for m in catalog.findall('.//message')}
for source, translation in messages.items():
    assert translation, f'Empty translation: {source}'
    assert collections.Counter(re.findall(r'%[1-9][0-9]*', source)) == collections.Counter(re.findall(r'%[1-9][0-9]*', translation)), source
    assert source.count('\n') == translation.count('\n'), f'Newlines differ: {source}'
for path in [root / 'qml/Main.qml', *root.glob('src/*.cpp')]:
    for match in re.finditer(r'(?:qsTranslate|QCoreApplication::translate)\("BookOrbit",\s*("(?:[^"\\]|\\.)*")', path.read_text()):
        source = json.loads(match[1])
        assert source in messages, f'Missing Russian translation in {path.name}: {source}'
print(f'PASS: {len(messages)} translations, source coverage, placeholders and line breaks')

lrelease = shutil.which('lrelease') or shutil.which('lrelease6')
if not lrelease and Path('/usr/lib/qt6/bin/lrelease').exists():
    lrelease = '/usr/lib/qt6/bin/lrelease'
if lrelease:
    with tempfile.TemporaryDirectory() as directory:
        for ts in (root / 'translations').glob('*.ts'):
            qm = Path(directory) / ts.with_suffix('.qm').name
            subprocess.run([lrelease, '-silent', str(ts), '-qm', str(qm)], check=True)
            assert qm.read_bytes() == ts.with_suffix('.qm').read_bytes(), f'Rebuild {ts.with_suffix(".qm").name}'
    print('PASS: compiled catalogs match their editable sources')
