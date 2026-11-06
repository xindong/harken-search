#!/usr/bin/env python3
"""Exercise the real Mac transfer path and native queue without moving the desktop pointer."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import wave

from macos_library_smoke import App, save


def make_wav(path, frequency):
    path.parent.mkdir(parents=True, exist_ok=True)
    samples = (struct.pack('<h', round(5000 * math.sin(2 * math.pi * frequency * i / 48000)))
               for i in range(48000 * 6))
    with wave.open(str(path), 'wb') as output:
        output.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
        output.writeframes(b''.join(samples))


def run(binary, root, output):
    output.mkdir(parents=True, exist_ok=False)
    fixture = root / 'fixtures' / output.name
    first = fixture / '钟声 1.wav'
    second = fixture / 'tone 2.wav'
    make_wav(first, 440)
    make_wav(second, 660)
    hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in (first, second)}
    app = App(binary, root, output)
    report = {'status': 'FAIL', 'scope': 'real Mac app: prepared file drag, rendered region, cancel, restart'}
    try:
        app.wait(lambda s: not s['switch_pending'], 'startup', 45)
        app.command('create', name='Outbound-' + output.name)
        app.wait(lambda s: s['database_open'] and not s['switch_pending'], 'library open', 45)
        app.command('scan', path=str(fixture), worker_threads=1)
        scanned = app.wait(lambda s: not s['scan']['active'] and not s['scan']['pending'] and
                           not s['search_pending'] and s['count'] == 2, 'two files scanned', 90)
        report['database'] = scanned['database']
        app.command('select', index=0)
        selected = app.wait(lambda s: s['audio']['loaded'] and s['audio']['wave_ready'],
                            'audio and waveform ready', 45)
        source = selected['audio']['path']
        assert source in hashes, selected['audio']

        queued = app.command('outbound_drag_probe', path=source)
        assert queued['outbound_drag_pending'] == 1, queued
        assert app.command('outbound_drag_cancel')['outbound_drag_pending'] == 0
        report['original_file_queued_and_cancelled'] = True

        app.command('region_set', start=0.2, end=0.6)
        before = set((root / 'temp').rglob('*.wav'))
        queued = app.command('outbound_drag_probe', path=source)
        assert queued['outbound_drag_pending'] == 1, queued
        rendered = set((root / 'temp').rglob('*.wav')) - before
        assert rendered, 'processed drag produced no isolated WAV copy'
        clip = next(iter(rendered))
        with wave.open(str(clip), 'rb') as stream:
            frames, rate = stream.getnframes(), stream.getframerate()
        assert 0 < frames < 48000 * 6 and rate == 48000, (frames, rate)
        report['rendered_copy'] = {'path': str(clip), 'frames': frames, 'rate': rate}
        assert app.command('outbound_drag_cancel')['outbound_drag_pending'] == 0

        first_copy_hash = hashlib.sha256(clip.read_bytes()).hexdigest()
        before = set((root / 'temp').rglob('*.wav'))
        queued = app.command('outbound_drag_probe', path=source)
        assert queued['outbound_drag_pending'] == 1, queued
        repeated = set((root / 'temp').rglob('*.wav')) - before
        assert (len(repeated) == 1 and
                hashlib.sha256(clip.read_bytes()).hexdigest() == first_copy_hash), \
            'repeat drag overwrote the earlier rendered copy'
        assert app.command('outbound_drag_cancel')['outbound_drag_pending'] == 0
        report['repeat_kept_first_rendered_copy'] = True

        before = set((root / 'temp').rglob('*.wav'))
        queued = app.command('outbound_region_drag_probe')
        assert queued['outbound_drag_pending'] == 1, queued
        assert set((root / 'temp').rglob('*.wav')) - before, 'waveform drag created no copy'
        assert app.command('outbound_drag_cancel')['outbound_drag_pending'] == 0
        report['waveform_region_queued_and_cancelled'] = True
        app.command(capture=True)
        report['normal_exit'] = app.close()
        app.retain()

        reopened = output / 'reopen'
        reopened.mkdir()
        app = App(binary, root, reopened)
        app.wait(lambda s: s['database'] == report['database'] and s['count'] == 2 and
                 not s['search_pending'], 'library restored after restart', 45)
        report['reopen_exit'] = app.close()
        assert all(hashlib.sha256(Path(path).read_bytes()).hexdigest() == digest
                   for path, digest in hashes.items()), 'source audio changed'
        report['sources_unchanged'] = True
        report['status'] = 'PASS'
    except Exception as error:
        report['error'] = str(error)
        try:
            report['exit_after_failure'] = app.close()
        except Exception as close_error:
            report['close_error'] = str(close_error)
            if app.process.poll() is None:
                app.process.terminate()
                app.process.wait(timeout=10)
                report['forced_exit'] = True
    finally:
        app.retain()
        save(output / 'report.json', report)
    print(json.dumps(report, ensure_ascii=False))
    return report['status'] == 'PASS'


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if any(not path.is_absolute() or path.resolve() != path for path in
           (args.app, args.root, args.out)):
        parser.error('absolute non-symlink paths required')
    if 'qa' not in args.root.parts or 'qa' not in args.out.parts:
        parser.error('isolated qa roots required')
    raise SystemExit(0 if run(args.app, args.root, args.out) else 1)
