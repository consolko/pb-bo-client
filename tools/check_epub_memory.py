"""Measure each XML workload in a fresh process, including transient peak RSS."""
from pathlib import Path
import os
import io
import random
import zipfile
import subprocess
import sys
import tempfile
from audit_fixtures import archive

with tempfile.TemporaryDirectory(prefix="bookorbit-xml-memory-") as directory:
    rng = random.Random(0)
    random_text = rng.randbytes(1024 * 1024).hex()
    cases = [
        ("large-text", archive("<p>" + "x" * (4 * 1024 * 1024) + "</p>"), True),
        ("large-deflate", archive("<p>" + random_text + "</p>"), True),
        ("oversize-dom", archive("<p>" + "x" * (7 * 1024 * 1024) + "</p>"), False),
        ("dense-dom", archive("<i>x</i>" * 60000), False),
        ("streamed-length", archive(second="<html><body><p>" + "x" * (7 * 1024 * 1024) + "</p></body></html>"), True),
    ]
    for name, data, expected in cases:
        path = Path(directory) / (name + ".epub")
        if name == "large-deflate":
            output = io.BytesIO()
            with zipfile.ZipFile(io.BytesIO(data)) as source, zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as target:
                for entry in source.namelist():
                    target.writestr(entry, source.read(entry))
            data = output.getvalue()
        path.write_bytes(data)
        subprocess.run([sys.argv[1], "--epub-memory", str(path), "1" if expected else "0"],
                       env=dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software"), check=True)
