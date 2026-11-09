#!/usr/bin/env python3
"""Click the real Mac metadata pickers in a generated, isolated audio library."""
import argparse
import json
from pathlib import Path
import sqlite3
import struct
import time
import wave

from macos_library_smoke import App, save

FIXTURE_COUNT = 50


def fixtures(root: Path):
    root.mkdir(parents=True, exist_ok=False)
    for index in range(FIXTURE_COUNT):
        path = root / f'picker-{index:02d}.wav'
        with wave.open(str(path), 'wb') as stream:
            stream.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
            stream.writeframes(struct.pack('<h', 1100 + index) * 9600)


def run(binary: Path, root: Path, output: Path):
    output.mkdir(parents=True, exist_ok=False)
    app = App(binary, root, output)
    report = {'status': 'FAIL', 'scope': 'Mac real UI picker list, draft and composed filters', 'steps': []}

    def snapshot():
        return app.command()

    def rect(key):
        state = app.wait(lambda s: key in s['qa_filter_ui_rects'], key, 12)
        return state['qa_filter_ui_rects'][key]

    def pointer(x, y):
        for phase in ('hover', 'down', 'up'):
            before = snapshot()['wave_ui']['frame']
            app.command('pointer', x=x, y=y, phase=phase)
            app.wait(lambda s: s['wave_ui']['frame'] > before, 'pointer frame', 8)

    def click(key):
        left, top, right, bottom = rect(key)
        pointer((left + right) / 2, (top + bottom) / 2)

    def popup(field):
        click('metadata_' + field + '_arrow')
        return rect('metadata_' + field + '_popup')

    def pick(field, index):
        popup(field)
        key = f'metadata_{field}_item_{index}'
        for attempt in range(8):
            state = snapshot()
            if key in state['qa_filter_ui_rects']:
                break
            left, top, right, bottom = rect('metadata_' + field + '_popup')
            app.command('pointer', x=(left + right) / 2, y=(top + bottom) / 2, phase='hover')
            app.command('wheel', delta=-4)
            time.sleep(0.1)
        click(key)
        state = app.wait(lambda s: not s['search_pending'] and
                         s['metadata_filters'][field.lower()] == values[field][index],
                         field + ' selected', 18)
        assert 'metadata_' + field + '_popup' not in state['qa_filter_ui_rects']
        return state

    def record(label, state):
        frame = app.command(capture=True)
        report['steps'].append({'label': label, 'count': state['count'],
                                'filters': state['metadata_filters'],
                                'screenshot': f'{frame["id"]:04d}.png'})

    values = {}
    try:
        app.wait(lambda s: not s['switch_pending'], 'startup mount', 60)
        app.command('create', name='Picker-' + output.name)
        created = app.wait(lambda s: s['database_open'] and not s['switch_pending']
                           and 'Picker-' + output.name in s['database'], 'create library', 40)
        report['library'] = created['database']
        source = root / 'fixtures' / output.name
        fixtures(source)
        app.command('scan', path=str(source))
        app.wait(lambda s: not s['scan']['active'] and not s['scan']['pending']
                 and not s['search_pending'] and s['count'] == FIXTURE_COUNT,
                 'scan generated files', 90)
        with sqlite3.connect(created['database']) as db:
            for index in range(FIXTURE_COUNT):
                category = ('QA Long Category ' + 'X' * 140 + '界') if index == FIXTURE_COUNT - 1 else f'Category {index:02d}'
                db.execute('UPDATE audio_files SET artist=?, album=?, category=? WHERE file_path=?',
                           (f'Artist {index:02d}', f'Album {index:02d}', category,
                            str(source / f'picker-{index:02d}.wav')))
            db.commit()
            for field, column in (('Artist', 'artist'), ('Album', 'album'), ('Category', 'category')):
                values[field] = [row[0] for row in db.execute(
                    f'SELECT DISTINCT {column} FROM audio_files WHERE {column} <> "" ORDER BY {column} COLLATE NOCASE')]
        assert all(len(items) == FIXTURE_COUNT for items in values.values()), values
        app.command('filter', text='')
        app.wait(lambda s: not s['search_pending'] and s['count'] == FIXTURE_COUNT,
                 'refresh updated rows', 25)

        app.command('sidebar', section=2)
        current = app.wait(lambda s: 'metadata_Artist_arrow' in s['qa_filter_ui_rects']
                           and s['active_sidebar_section'] == 2, 'metadata panel visible', 15)
        report['window'] = current['window']
        for field in ('Artist', 'Album', 'Category'):
            bounds = popup(field)
            assert bounds[2] > bounds[0] and bounds[3] > bounds[1], (field, bounds)
            assert f'metadata_{field}_item_0' in snapshot()['qa_filter_ui_rects']
            record(field.lower() + '-expanded', snapshot())
            click(f'metadata_{field}_item_0')
            current = app.wait(lambda s: not s['search_pending'] and
                               s['metadata_filters'][field.lower()] == values[field][0],
                               field + ' composed', 18)
            assert current['count'] == 1, (field, current['count'])
        record('three-filters-composed', current)

        current = pick('Artist', 1)
        assert current['count'] == 0 and current['metadata_filters']['album'] == values['Album'][0]
        assert current['metadata_filters']['category'] == values['Category'][0]
        record('change-artist-preserves-album-category', current)
        popup('Artist')
        click('metadata_Artist_all')
        current = app.wait(lambda s: not s['search_pending'] and
                           s['metadata_filters']['artist'] == '' and s['count'] == 1,
                           'artist All clears only artist', 18)
        assert current['metadata_filters']['album'] == values['Album'][0]
        assert current['metadata_filters']['category'] == values['Category'][0]

        click('metadata_Artist_input')
        app.wait(lambda s: 'metadata_Artist_popup' in s['qa_filter_ui_rects'], 'input opens list')
        app.command('text', text='no-match-987')
        draft = app.wait(lambda s: s['metadata_draft_query'].get('Artist') == 'no-match-987',
                         'query edits list only', 10)
        assert draft['metadata_filters']['artist'] == ''
        assert not any(k.startswith('metadata_Artist_item_') for k in draft['qa_filter_ui_rects'])
        bounds = draft['qa_filter_ui_rects']['metadata_Artist_popup']
        x = bounds[2] + 20 if bounds[2] + 20 < draft['window']['width'] else bounds[0] - 20
        pointer(x, min(bounds[1] + 80, draft['window']['height'] - 20))
        closed = app.wait(lambda s: 'metadata_Artist_popup' not in s['qa_filter_ui_rects'],
                          'outside click closes without applying', 10)
        assert closed['metadata_filters'] == draft['metadata_filters']
        record('draft-cancelled-outside', closed)

        click('metadata_Artist_input')
        app.wait(lambda s: 'metadata_Artist_popup' in s['qa_filter_ui_rects'],
                 'reopen artist query')
        app.command('text', text='Artist 2')
        matches = app.wait(lambda s: s['metadata_draft_query'].get('Artist') == 'Artist 2'
                           and 'metadata_Artist_item_20' in s['qa_filter_ui_rects'],
                           'positive name search', 10)
        assert matches['metadata_filters']['artist'] == ''
        click('metadata_Artist_item_20')
        matched = app.wait(lambda s: not s['search_pending'] and
                           s['metadata_filters']['artist'] == 'Artist 20',
                           'query selection applies exact name', 18)
        assert matched['count'] == 0 and matched['metadata_filters']['album'] == values['Album'][0]
        record('positive-query-selected', matched)
        popup('Artist')
        click('metadata_Artist_all')
        app.wait(lambda s: not s['search_pending'] and s['metadata_filters']['artist'] == '',
                 'clear artist after query', 18)

        popup('Album')
        click('metadata_Album_all')
        app.wait(lambda s: not s['search_pending'] and s['metadata_filters']['album'] == '',
                 'clear album', 18)
        long_index = values['Category'].index('QA Long Category ' + 'X' * 140 + '界')
        long_selected = pick('Category', long_index)
        assert long_selected['count'] == 1 and len(long_selected['metadata_filters']['category'].encode()) > 127
        record('long-category-exactly-applied', long_selected)

        fresh_name = 'Fresh Added Artist'
        with sqlite3.connect(created['database']) as db:
            db.execute('UPDATE audio_files SET artist=? WHERE file_path=?',
                       (fresh_name, str(source / 'picker-01.wav')))
        click('metadata_Artist_input')
        app.wait(lambda s: 'metadata_Artist_popup' in s['qa_filter_ui_rects'],
                 'reopen picker after library change')
        app.command('text', text=fresh_name)
        fresh = app.wait(lambda s: s['metadata_draft_query'].get('Artist') == fresh_name and
                         any(k.startswith('metadata_Artist_item_') for k in s['qa_filter_ui_rects']),
                         'new library name is visible', 10)
        options = [key for key in fresh['qa_filter_ui_rects'] if key.startswith('metadata_Artist_item_')]
        assert len(options) == 1, options
        click(options[0])
        updated = app.wait(lambda s: not s['search_pending'] and
                           s['metadata_filters']['artist'] == fresh_name,
                           'new name can be selected', 18)
        assert updated['metadata_filters']['category'] == long_selected['metadata_filters']['category']
        record('new-name-after-reopen', updated)

        report['exit_code'] = app.close()
        app.retain()
        reopened = output / 'reopen'
        reopened.mkdir()
        app = App(binary, root, reopened)
        restored = app.wait(lambda s: s['database'] == report['library'] and
                            not s['switch_pending'] and not s['search_pending'] and
                            s['count'] == FIXTURE_COUNT,
                            'reopen same independent library', 60)
        app.command('sidebar', section=2)
        app.wait(lambda s: 'metadata_Category_arrow' in s['qa_filter_ui_rects'], 'picker after reopen')
        report['reopen'] = {'library': restored['database'], 'count': restored['count']}
        report['reopen_exit_code'] = app.close()
        report['status'] = 'PASS'
    except Exception as error:
        report['error'] = str(error)
        try:
            app.command(capture=True)
        except Exception as capture_error:
            report['capture_error'] = str(capture_error)
        try:
            report['exit_code'] = app.close()
        except Exception as close_error:
            report['shutdown_error'] = str(close_error)
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
    for path in (args.app, args.root, args.out):
        if not path.is_absolute() or path.resolve() != path:
            parser.error('absolute non-symlink path required')
    if 'qa' not in args.root.parts or 'qa' not in args.out.parts:
        parser.error('dedicated qa roots required')
    raise SystemExit(0 if run(args.app, args.root, args.out) else 1)
