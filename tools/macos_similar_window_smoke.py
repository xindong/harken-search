#!/usr/bin/env python3
"""Exercise the real Similar Sounds window in the isolated Mac QA app."""
import argparse
import math
from pathlib import Path
import struct
import wave

from macos_library_smoke import App, save


def make_audio(root: Path) -> None:
    root.mkdir(parents=True, exist_ok=False)
    for index, frequency in enumerate((220, 232, 440, 454, 680, 692)):
        path = root / f'tone-{index + 1:02d}.wav'
        with wave.open(str(path), 'wb') as output:
            output.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
            samples = (round(6000 * math.sin(2 * math.pi * frequency * frame / 48000))
                       for frame in range(48000 * 2))
            output.writeframes(b''.join(struct.pack('<h', sample) for sample in samples))


def click(app: App, rect) -> None:
    x, y = (rect[0] + rect[2]) / 2, (rect[1] + rect[3]) / 2
    app.command('pointer', x=x, y=y, phase='hover')
    app.command('pointer', x=x, y=y, phase='down')
    app.command('pointer', x=x, y=y, phase='up')


def run(binary: Path, root: Path, output: Path) -> None:
    output.mkdir(parents=True, exist_ok=False)
    report = {'status': 'FAIL', 'scope': 'real Similar Sounds window'}
    app = App(binary, root, output)
    try:
        app.wait(lambda s: not s['switch_pending'], 'startup mount', 60)
        name = 'Mac-' + output.name
        app.command('create', name=name)
        app.wait(lambda s: s['database_open'] and not s['switch_pending'] and name in s['database'],
                 'create test library')
        fixture_root = root / 'fixtures' / output.name
        make_audio(fixture_root)
        app.command('scan', path=str(fixture_root))
        app.wait(lambda s: not s['scan']['active'] and not s['scan']['pending']
                 and s['scan']['discovered'] == 6, 'scan completion', 90)
        app.wait(lambda s: not s['search_pending'] and len(s['rows']) == 6,
                 'six visible results')
        app.command('select', index=0)
        selected = app.wait(lambda s: s['audio']['selected_id'] > 0,
                            'audio selection')
        reference_id = selected['audio']['selected_id']
        before = app.command(capture=True)
        entry = before['similar_ui']['zones'].get('info.similarSounds')
        assert entry, f'Info Similar Sounds entry unavailable: {before["similar_ui"]}'
        click(app, entry)
        opened = app.wait(lambda s: s['similar_ui']['visible'] and
                          s['similar_ui'].get('reference_id') == reference_id,
                          'Similar Sounds window and reference')
        app.command(capture=True)
        report.update(library=opened['database'], reference_id=reference_id,
                      opened=opened['similar_ui'])
        ready = app.wait(lambda s: 'similar.index.start' in s['similar_ui']['zones'],
                         'index action visible', 30)
        click(app, ready['similar_ui']['zones']['similar.index.start'])
        indexed = app.wait(lambda s: not s['similar_ui']['index_running'] and
                           s['similar_ui']['index_report']['available'] and
                           not s['similar_ui']['index_report']['running'] and
                           s['similar_ui']['index_report']['total'] == 6,
                           'analysis finished', 90)
        report['index'] = indexed['similar_ui']['index_report']
        app.command(capture=True)
        assert report['index']['succeeded'] == 6, report['index']
        preset = indexed['similar_ui']['zones']['similar.options.preset']
        # The Find button immediately follows the fixed 132-point preset control.
        # Validate the resulting workflow state, not the click coordinate alone.
        find_x = preset[2] + 56
        find_y = (preset[1] + preset[3]) / 2
        assert find_x < indexed['window']['width']
        click(app, (find_x, find_y, find_x, find_y))
        result = app.wait(lambda s: s['similar_ui'].get('status') in (3, 4),
                          'Find result or explicit error', 60)
        app.command(capture=True)
        report['query'] = result['similar_ui']
        if result['similar_ui']['status'] == 3 and result['similar_ui']['rows']:
            file_zone = result['similar_ui']['zones'].get('similar.result.file.0')
            assert file_zone, 'first result has no clickable row'
            click(app, file_zone)
            selected_result = app.wait(
                lambda s: s['similar_ui']['selected_row'] == 0 and
                          s['audio']['path'] == result['similar_ui']['rows'][0]['path'],
                'result row audition', 30)
            assert selected_result['audio']['selected_id'] == reference_id, +                'result selection changed the main list selection'
            report['audition'] = selected_result['audio']
            app.command(capture=True)
            report['status'] = 'PASS'
        else:
            report['status'] = 'QUERY_UNAVAILABLE'
        report['exit_code'] = app.close()
    except Exception as error:
        report['error'] = repr(error)
        if app.process.poll() is None:
            try:
                report['exit_code'] = app.close()
            except Exception as close_error:
                report['close_error'] = repr(close_error)
        raise
    finally:
        app.retain()
        save(output / 'report.json', report)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = run(args.binary, args.root, args.output)
    print(f"Similar Sounds: {result['status']}; "
          f"query error={result.get('query', {}).get('error')}; "
          f"index model error={result.get('index', {}).get('model_error')}")
    raise SystemExit(0 if result['status'] == 'PASS' else 2)
