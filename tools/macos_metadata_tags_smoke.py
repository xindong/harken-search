#!/usr/bin/env python3
"""Inspect one-file metadata/tag persistence in the isolated real Mac app."""
import argparse
import hashlib
import json
from pathlib import Path
import sqlite3

from macos_library_smoke import App, save
from macos_search_composition_smoke import make_fixtures


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def edited_row(snapshot, name):
    rows = [row for row in snapshot['rows'] if row['name'] == name]
    assert len(rows) == 1, (name, snapshot['rows'])
    return rows[0]


def capture_matching(app, predicate, label):
    for _ in range(10):
        frame = app.command(capture=True)
        if predicate(frame):
            return frame
        app.wait(predicate, label)
    raise AssertionError(f'{label}: no settled captured frame')


def select_one(app, name):
    # A count reply can be followed by another refresh that briefly clears rows.
    for _ in range(10):
        snapshot = app.wait(lambda s: not s['search_pending'] and s['count'] == 1
                            and len(s['rows']) == 1 and s['rows'][0]['name'] == name,
                            'one settled row')
        try:
            app.command('select', index=0)
            return snapshot['rows'][0]['id']
        except AssertionError as error:
            if 'invalid_selection' not in str(error):
                raise
    raise AssertionError('selection did not settle')


def tag_one(app, name, tags):
    for _ in range(10):
        app.wait(lambda s: not s['search_pending']
                 and any(row['name'] == name for row in s['rows']), 'tag target visible')
        try:
            app.command('tag', file_name=name, tags=tags)
            return
        except AssertionError as error:
            if 'row_not_found' not in str(error):
                raise
    raise AssertionError('tag target did not settle')


def run(binary, root, output):
    output.mkdir(parents=True, exist_ok=False)
    app = App(binary, root, output)
    report = {'status': 'FAIL', 'scope': 'single-file metadata and tag persistence in real Mac app', 'checks': []}
    try:
        app.wait(lambda s: not s['switch_pending'], 'startup', 60)
        library_name = 'Metadata-' + output.name
        app.command('create', name=library_name)
        opened = app.wait(lambda s: s['database_open'] and library_name in s['database']
                          and not s['switch_pending'], 'create library')
        report['library'] = opened['database']
        source = root / 'fixtures' / output.name
        make_fixtures(source)
        target = source / 'Cinematic/war gun.wav'
        original_hash = digest(target)
        app.command('scan', path=str(source))
        app.wait(lambda s: not s['scan']['active'] and not s['scan']['pending']
                 and s['scan']['discovered'] >= 6, 'scan completion', 90)
        app.wait(lambda s: not s['search_pending'] and s['count'] == 6
                 and len(s['rows']) == 6, 'six files')

        values = {'description': 'Evening storm ambience',
                  'keywords': 'storm pulse', 'category': 'Nature'}
        app.command('metadata_patch', path=str(target), fields=values)
        updated = app.wait(lambda s: not s['search_pending'] and s['count'] == 6 and len(s['rows']) == 6
                           and all(edited_row(s, target.name).get(k) == v for k, v in values.items()),
                           'metadata patch')
        report['checks'].append({'step': 'single file patch', 'fields': values})
        final_values = dict(values, description='Evening storm ambience revised')
        app.command('metadata_patch', path=str(target), fields={'description': final_values['description']})
        app.wait(lambda s: not s['search_pending'] and s['count'] == 6 and len(s['rows']) == 6
                 and all(edited_row(s, target.name).get(k) == v for k, v in final_values.items()),
                 'description-only patch preserves keywords and category')
        report['checks'].append({'step': 'one-field patch preserves other fields', 'fields': final_values})

        for path, fields, error_text in [(target, {'unlisted_field': 'should not save'}, 'unsupported_metadata_field'),
                                         (source / 'missing.wav', {'description': 'should not save'}, 'row_not_found')]:
            try:
                app.command('metadata_patch', path=str(path), fields=fields)
                raise AssertionError(f'accepted invalid patch: {path}, {fields}')
            except AssertionError as error:
                assert error_text in str(error), str(error)
        unchanged = app.command()
        assert all(edited_row(unchanged, target.name).get(k) == v for k, v in final_values.items())
        report['checks'].append({'step': 'invalid patches rejected without changing existing fields'})

        app.command('filter', text='Nature')
        app.wait(lambda s: not s['search_pending'] and s['count'] == 1
                 and len(s['rows']) == 1 and s['rows'][0]['name'] == target.name,
                 'category remains searchable after description-only patch')
        app.command('filter', text='')
        app.wait(lambda s: not s['search_pending'] and s['count'] == 6 and len(s['rows']) == 6,
                 'clear category query before tag')
        tag_one(app, target.name, ['atmosphere'])
        app.command('filter', text='storm tag:atmosphere', folder=str(source / 'Cinematic'))
        found = app.wait(lambda s: not s['search_pending'] and s['count'] == 1
                         and len(s['rows']) == 1 and edited_row(s, target.name)['keywords'] == final_values['keywords'],
                         'metadata and tag search')
        selected_id = select_one(app, target.name)
        app.wait(lambda s: s['audio']['selected_id'] == selected_id
                 and s['audio']['loaded'], 'selected file info', 30)
        app.command('info_metadata_tab')
        frame = capture_matching(app, lambda s: not s['search_pending'] and s['count'] == 1
                                 and len(s['rows']) == 1 and s['audio']['selected_id'] == selected_id,
                                 'metadata screenshot')
        report['checks'].append({'step': 'composed search and selection', 'screenshot': f'{frame["id"]:04d}.png'})
        assert digest(target) == original_hash, 'source audio changed during database-only edit'

        with sqlite3.connect(Path(report['library']).as_uri() + '?mode=ro', uri=True) as connection:
            stored = connection.execute('SELECT description, keywords, category FROM audio_files WHERE file_path=?',
                                        (str(target),)).fetchone()
        assert stored == tuple(final_values[k] for k in ('description', 'keywords', 'category')), stored
        report['checks'].append({'step': 'database fields', 'stored': stored, 'source_sha256': original_hash})
        report['first_exit_code'] = app.close()
        app.retain()

        reopen = output / 'reopen'
        reopen.mkdir()
        app = App(binary, root, reopen)
        restored = app.wait(lambda s: s['database'] == report['library'] and not s['switch_pending']
                            and not s['search_pending'] and s['count'] == 6 and len(s['rows']) == 6,
                            'reopen library', 60)
        assert all(edited_row(restored, target.name).get(k) == v for k, v in final_values.items())
        app.command('filter', text='storm tag:atmosphere', folder=str(source / 'Cinematic'))
        after = app.wait(lambda s: not s['search_pending'] and s['count'] == 1
                         and len(s['rows']) == 1 and s['rows'][0]['name'] == target.name,
                         'search after reopen')
        selected_id = select_one(app, target.name)
        app.wait(lambda s: s['audio']['selected_id'] == selected_id and s['audio']['loaded'],
                 'selection after reopen', 30)
        app.command('info_metadata_tab')
        frame = capture_matching(app, lambda s: not s['search_pending'] and s['count'] == 1
                                 and len(s['rows']) == 1 and s['audio']['selected_id'] == selected_id,
                                 'reopen screenshot')
        report['checks'].append({'step': 'reopen preserves fields and tag search',
                                 'screenshot': 'reopen/' + f'{frame["id"]:04d}.png'})
        assert digest(target) == original_hash
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
    for path in [args.app, args.root, args.out]:
        if not path.is_absolute() or path.resolve() != path:
            parser.error('absolute non-symlink paths required')
    if 'qa' not in args.root.parts or 'qa' not in args.out.parts:
        parser.error('dedicated qa roots required')
    raise SystemExit(0 if run(args.app, args.root, args.out) else 1)
