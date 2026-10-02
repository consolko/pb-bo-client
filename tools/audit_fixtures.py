"""Small reproducible EPUBs for XML, logical CFI and replacement regressions."""
import io
from pathlib import Path
import zipfile


def archive(body='<p id="p">abcd</p>', *, doctype="", second=None, missing=None, container=None):
    output = io.BytesIO()
    package = ('<package xmlns="http://www.idpf.org/2007/opf" version="3.0">'
               '<metadata/><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>'
               + ('<item id="second" href="second.xhtml" media-type="application/xhtml+xml"/>' if second is not None else '')
               + '</manifest><spine><itemref id="chapter-ref" idref="chapter"/>'
               + ('<itemref idref="second"/>' if second is not None else '') + '</spine></package>')
    contents = {
        'mimetype': 'application/epub+zip',
        'META-INF/container.xml': container or '<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="book.opf" media-type="application/oebps-package+xml"/></rootfiles></container>',
        'book.opf': package,
        'chapter.xhtml': '<?xml version="1.0"?>' + doctype + '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Test</title></head><body>' + body + '</body></html>',
    }
    if second is not None:
        contents['second.xhtml'] = second
    if missing:
        contents.pop(missing)
    with zipfile.ZipFile(output, 'w') as result:
        for name, value in contents.items():
            result.writestr(zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0)), value)
    return output.getvalue()


def ordinary_zip():
    output = io.BytesIO()
    with zipfile.ZipFile(output, 'w') as result:
        result.writestr('unrelated.txt', 'This archive is not an EPUB.')
    return output.getvalue()


def write_fixtures(directory: Path) -> None:
    cases = {
        'plain': archive(),
        'doctype': archive(doctype='<!DOCTYPE html>'),
        'comment': archive('<p id="p">ab<!--ignored-->cd</p>'),
        'cdata': archive('<p id="p">a<![CDATA[bc]]>d</p>'),
        'empty_chunk': archive('<p id="p"><em>x</em></p>'),
        'unicode': archive('<p id="p">a\U0001f680b</p>'),
        'whitespace': archive('<p id="p">  <!--ignored--> </p>'),
        'unrelated': archive(second='<not-well-formed>'),
        'external': archive(doctype='<!DOCTYPE html SYSTEM "file:///etc/passwd">'),
        'entities': archive('<p>&secret;</p>', doctype='<!DOCTYPE html [<!ENTITY secret SYSTEM "file:///etc/passwd">]>'),
        'internal': archive('<p>&word;</p>', doctype='<!DOCTYPE html [<!ENTITY word "text">]>'),
        'deep': archive('<div>' * 130 + 'text' + '</div>' * 130),
        'missing_container': archive(missing='META-INF/container.xml'),
        'missing_opf': archive(missing='book.opf'),
        'missing_spine': archive(missing='chapter.xhtml'),
        'ordinary_zip': ordinary_zip(),
        'truncated_zip': archive()[:-15],
    }
    for name, raw in cases.items():
        (directory / (name + '.epub')).write_bytes(raw)
