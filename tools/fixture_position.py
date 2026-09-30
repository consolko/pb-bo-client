"""Resolve only the simple CFI subset emitted for mock_bookorbit's controlled EPUBs."""
import argparse
import io
import json
from pathlib import Path
import re
from xml.etree import ElementTree as ET
import zipfile


def resolve(data, position):
    # ponytail: no general EPUB CFI parser; reject assertions/ranges/other spines explicitly.
    match = re.fullmatch(r'(?:pbr:/webkit\?##)?epubcfi\(/6/2!/4/(\d+)/1(?::(\d+))?\)', position)
    if not match:
        raise ValueError('Unsupported fixture CFI')
    step, offset = int(match[1]), int(match[2] or '0')
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        package = ET.fromstring(archive.read('book.opf'))
        ns = {'o': 'http://www.idpf.org/2007/opf'}
        spine = package.find('o:spine', ns)
        if spine is None or len(spine) != 1 or spine[0].get('idref') != 'chapter':
            raise ValueError('Not the controlled fixture spine')
        chapter = ET.fromstring(archive.read('chapter.xhtml'))
    body = chapter.find('{http://www.w3.org/1999/xhtml}body')
    if body is None or step < 2 or step % 2 or step//2 > len(body):
        raise ValueError('Invalid element step')
    element = body[step//2 - 1]
    if len(element) or not re.fullmatch(r'p\d+', element.get('id', '')):
        raise ValueError('Not a plain fixture paragraph')
    text = element.text or ''
    encoded = text.encode('utf-16-le')
    if offset * 2 > len(encoded):
        raise ValueError('Text offset exceeds paragraph')
    # Strict decode also rejects an offset inside a surrogate pair.
    before = encoded[:offset*2].decode('utf-16-le')
    after = encoded[offset*2:].decode('utf-16-le')
    return {'paragraph': element.get('id'), 'utf16_offset': offset, 'before': before, 'after': after}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('epub', type=Path)
    parser.add_argument('position')
    args = parser.parse_args()
    print(json.dumps(resolve(args.epub.read_bytes(), args.position), ensure_ascii=False, indent=2))
