#!/usr/bin/env python3
"""Verify WAV region export in the isolated real Mac application."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import time
import wave

from macos_library_smoke import App, save


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def fixture(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    frames = bytearray()
    for index in range(96000):
        frequency = (220, 330, 440, 550)[index // 24000]
        sample = round(6000 * math.sin(2 * math.pi * frequency * index / 48000))
        frames.extend(struct.pack('<hh', sample, -sample))
    with wave.open(str(path), 'wb') as output:
        output.setparams((2, 2, 48000, 0, 'NONE', 'not compressed'))
        output.writeframes(frames)


def dominant_zero_crossings(samples):
    # A 100 ms window contains roughly frequency / 10 positive crossings.
    return sum(a <= 0 < b for a, b in zip(samples, samples[1:])) * 10


def inspect_export(path):
    with wave.open(str(path), 'rb') as stream:
        channels, width, rate, frames = (stream.getnchannels(), stream.getsampwidth(),
                                          stream.getframerate(), stream.getnframes())
        payload = stream.readframes(frames)
    assert (channels, width, rate, frames) == (2, 3, 48000, 48000), (channels, width, rate, frames)
    left = []
    for frame in range(frames):
        offset = frame * channels * width
        left.append(int.from_bytes(payload[offset:offset + 3], 'little', signed=True))
    first = dominant_zero_crossings(left[12000:16800])
    second = dominant_zero_crossings(left[36000:40800])
    assert abs(first - 330) <= 20, first
    assert abs(second - 440) <= 20, second
    return {'channels': channels, 'sample_width_bytes': width, 'sample_rate': rate,
            'frames': frames, 'duration_seconds': frames / rate,
            'first_segment_hz': first, 'second_segment_hz': second}


def run(binary, root, output, reproduce):
    output.mkdir(parents=True, exist_ok=False)
    app = App(binary, root, output)
    report = {'status': 'FAIL', 'scope': 'isolated real Mac app WAV region export', 'checks': []}
    try:
        app.wait(lambda s: not s['switch_pending'], 'startup', 60)
        library_name = 'Region-' + output.name
        app.command('create', name=library_name)
        created = app.wait(lambda s: s['database_open'] and not s['switch_pending']
                           and library_name in s['database'], 'create library')
        report['library'] = created['database']
        source = root / 'fixtures' / output.name / 'region-source.wav'
        fixture(source)
        before = digest(source)
        app.command('scan', path=str(source.parent))
        app.wait(lambda s: not s['scan']['active'] and not s['scan']['pending']
                 and s['scan']['discovered'] == 1, 'scan complete', 90)
        for _ in range(10):
            app.wait(lambda s: not s['search_pending'] and s['count'] == 1
                     and len(s['rows']) == 1, 'one visible row')
            try:
                app.command('select', index=0)
                break
            except AssertionError as error:
                if 'invalid_selection' not in str(error):
                    raise
        else:
            raise AssertionError('selection did not settle')
        app.wait(lambda s: s['audio']['loaded'] and s['audio']['path'] == str(source),
                 'audio loaded', 30)
        selected = app.command('region_set', start=0.25, end=0.75, fade_in=0.03, fade_out=0.04)
        assert abs(selected['region']['start'] - 0.25) < 1e-6
        assert abs(selected['region']['end'] - 0.75) < 1e-6
        app.wait(lambda s: s['audio']['wave_ready'] and not s['audio']['wave_pending'],
                 'selected waveform ready', 30)
        time.sleep(0.5)
        frame = app.command(capture=True)
        report['checks'].append({'step': 'real window and selected region',
                                 'screenshot': f'{frame["id"]:04d}.png'})

        same = app.command('region_export', path=str(source))['region_export']
        after_same = digest(source)
        report['checks'].append({'step': 'source path export', 'result': same,
                                 'source_changed': after_same != before})
        if reproduce:
            assert after_same != before, 'source overwrite did not reproduce'
            report['status'] = 'BUG_REPRODUCED'
        else:
            assert not same['ok'] and same['error'], same
            assert after_same == before, 'same-path rejection changed source audio'
            output_wav = root / 'exports' / output.name / 'selected.wav'
            output_wav.parent.mkdir(parents=True, exist_ok=False)
            alias = output_wav.parent / 'source-alias.wav'
            os.link(source, alias)
            alias_result = app.command('region_export', path=str(alias))['region_export']
            assert not alias_result['ok'] and alias_result['error'], alias_result
            assert digest(source) == before, 'hard-link alias changed source audio'
            report['checks'].append({'step': 'source hard-link alias export rejected',
                                     'result': alias_result})
            result = app.command('region_export', path=str(output_wav))['region_export']
            assert result['ok'] and not result['error'], result
            assert result['start_frame'] == 24000 and result['frames_written'] == 48000, result
            info = inspect_export(output_wav)
            report['checks'].append({'step': 'valid export', 'result': result,
                                     'wav': info, 'sha256': digest(output_wav)})
            bad_path = root / 'exports' / output.name / 'missing' / 'cannot-write.wav'
            failed = app.command('region_export', path=str(bad_path))['region_export']
            assert not failed['ok'] and failed['error'] and not bad_path.exists(), failed
            report['checks'].append({'step': 'unwritable destination', 'result': failed})
            try:
                app.command('region_export', path='/tmp/cindy-region-outside.wav')
                raise AssertionError('out-of-isolation output accepted')
            except AssertionError as error:
                assert 'path_outside_isolated_root' in str(error), str(error)
            try:
                app.command('region_set', start=0.8, end=0.2)
                raise AssertionError('invalid region accepted')
            except AssertionError as error:
                assert 'invalid_region' in str(error), str(error)
            assert digest(source) == before, 'source audio changed'
            report['checks'].append({'step': 'invalid inputs leave source intact',
                                     'source_sha256': before})
            report['status'] = 'PASS'
        report['exit_code'] = app.close()
        if not reproduce:
            app.retain()
            reopen = output / 'reopen'
            reopen.mkdir()
            app = App(binary, root, reopen)
            app.wait(lambda s: s['database'] == report['library'] and not s['switch_pending']
                     and not s['search_pending'] and s['count'] == 1, 'reopen library', 60)
            for _ in range(10):
                app.wait(lambda s: not s['search_pending'] and len(s['rows']) == 1,
                         'reopen visible row')
                try:
                    app.command('select', index=0)
                    break
                except AssertionError as error:
                    if 'invalid_selection' not in str(error):
                        raise
            else:
                raise AssertionError('reopen selection did not settle')
            app.wait(lambda s: s['audio']['loaded'] and s['audio']['path'] == str(source),
                     'reopen source', 30)
            app.wait(lambda s: s['audio']['wave_ready'] and not s['audio']['wave_pending'],
                     'reopen waveform ready', 30)
            time.sleep(0.5)
            frame = app.command(capture=True)
            assert digest(source) == before and inspect_export(output_wav)['frames'] == 48000
            report['checks'].append({'step': 'reopen source and exported WAV',
                                     'screenshot': 'reopen/' + f'{frame["id"]:04d}.png'})
            report['reopen_exit_code'] = app.close()
    except Exception as error:
        report['status'] = 'FAIL'
        report['error'] = str(error)
        try:
            report['exit_code'] = app.close()
        except Exception as close_error:
            report['close_error'] = str(close_error)
            if app.process.poll() is None:
                app.process.terminate()
                app.process.wait(timeout=10)
    finally:
        app.retain()
        save(output / 'report.json', report)
    print(json.dumps(report, ensure_ascii=False))
    return report['status'] == ('BUG_REPRODUCED' if reproduce else 'PASS')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--reproduce-source-overwrite', action='store_true')
    args = parser.parse_args()
    for path in [args.app, args.root, args.out]:
        if not path.is_absolute() or path.resolve() != path:
            parser.error('absolute non-symlink paths required')
    if 'qa' not in args.root.parts or 'qa' not in args.out.parts:
        parser.error('dedicated qa roots required')
    raise SystemExit(0 if run(args.app, args.root, args.out, args.reproduce_source_overwrite) else 1)
