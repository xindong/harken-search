#!/usr/bin/env python3
"""Generate short synthetic M4A fixtures under an isolated QA directory."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import wave

parser = argparse.ArgumentParser()
parser.add_argument('root', type=Path)
args = parser.parse_args()
root = args.root
if not root.is_absolute() or root.resolve() != root or 'qa' not in root.parts:
    parser.error('fixture root must be an absolute, non-symlink QA path')
root.mkdir(parents=True, exist_ok=False)
source = root / '基线 tone.wav'
rate = 44100
frames = rate * 3
with wave.open(str(source), 'wb') as sound:
    sound.setparams((2, 2, rate, frames, 'NONE', 'not compressed'))
    chunk = bytearray()
    for index in range(frames):
        left = round(6500 * math.sin(2 * math.pi * 440 * index / rate))
        right = round(4100 * math.sin(2 * math.pi * 660 * index / rate))
        chunk.extend(struct.pack('<hh', left, right))
    sound.writeframes(chunk)
outputs = {'wav': source.name}
for label, fmt, data, name in [
    ('aac_m4a', 'm4af', 'aac ', '基线 AAC.m4a'),
    ('alac_m4a', 'm4af', 'alac', '基线 ALAC.m4a'),
    ('aiff', 'AIFF', 'BEI16', '基线 AIFF.aiff'),
    ('flac', 'flac', 'flac', '基线 FLAC.flac'),
]:
    target = root / name
    cmd = ['/usr/bin/afconvert', '-f', fmt, '-d', data, str(source), str(target)]
    result = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    print(json.dumps({'label': label, 'cmd': cmd, 'code': result.returncode,
                      'stdout': result.stdout, 'stderr': result.stderr}, ensure_ascii=False))
    if result.returncode or not target.is_file() or not target.stat().st_size:
        raise RuntimeError(label + ' conversion failed')
    outputs[label] = name
(root / 'damaged.m4a').write_bytes(b'not an m4a file')
manifest = {label: {'name': name, 'bytes': (root / name).stat().st_size,
                    'sha256': hashlib.sha256((root / name).read_bytes()).hexdigest()}
            for label, name in outputs.items()}
(root / 'fixture-manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + chr(10), encoding='utf-8')
print(json.dumps(manifest, ensure_ascii=False))
