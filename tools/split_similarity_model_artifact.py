"""Split a verified CLAP export into small, independently checked CI artifacts.

Temporary CV-0EC12D84 transfer aid. The export script must succeed first.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import zipfile


FILES = (
    'clap-audio.onnx', 'clap-mel-filters.f64', 'clap-window.f64',
    'parity-audio.f32', 'parity-mel.f32', 'parity-embedding.f32',
    'export-report.json',
)
CHUNK_BYTES = 16 * 1024 * 1024


def digest(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open('rb') as stream:
        while block := stream.read(1024 * 1024):
            sha.update(block)
    return sha.hexdigest()


def main(source: Path, output: Path) -> None:
    output.mkdir(parents=True, exist_ok=False)
    archive = output / 'verified-clap.zip'
    files = []
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_STORED) as package:
        for name in FILES:
            path = source / name
            if not path.is_file():
                raise FileNotFoundError(path)
            package.write(path, arcname=name)
            files.append({'name': name, 'bytes': path.stat().st_size, 'sha256': digest(path)})
    parts = []
    with archive.open('rb') as stream:
        for index in range(8):
            block = stream.read(CHUNK_BYTES)
            if not block:
                raise RuntimeError('Verified export produced fewer than eight parts')
            part = output / f'part-{index:02d}.bin'
            part.write_bytes(block)
            parts.append({'name': part.name, 'bytes': len(block), 'sha256': digest(part)})
        if stream.read(1):
            raise RuntimeError('Verified export produced more than eight parts')
    manifest = {'archiveBytes': archive.stat().st_size, 'archiveSha256': digest(archive),
                'files': files, 'parts': parts}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    main(args.source, args.output)
