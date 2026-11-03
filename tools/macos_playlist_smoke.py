#!/usr/bin/env python3
"""Inspect playlist organization in the isolated real Mac product window."""
import argparse
import json
from pathlib import Path

from macos_library_smoke import App, save
from macos_search_composition_smoke import make_fixtures


def names(snapshot):
    return sorted(row['name'] for row in snapshot['rows'])


def playlist(snapshot, title):
    matches = [row for row in snapshot['playlists'] if row['name'] == title]
    assert len(matches) == 1, (title, snapshot['playlists'])
    return matches[0]


def run(binary, root, output):
    output.mkdir(parents=True, exist_ok=False)
    app = App(binary, root, output)
    report = {'status': 'FAIL', 'scope': 'real Mac app playlist organization and restart', 'checks': []}
    try:
        app.wait(lambda s: not s['switch_pending'], 'startup', 60)
        library_name = 'Playlist-' + output.name
        app.command('create', name=library_name)
        opened = app.wait(lambda s: s['database_open'] and library_name in s['database'] and not s['switch_pending'], 'create library')
        report['library'] = opened['database']
        source = root / 'fixtures' / output.name
        make_fixtures(source)
        app.command('scan', path=str(source))
        app.wait(lambda s: not s['scan']['active'] and not s['scan']['pending'] and s['scan']['discovered'] >= 6, 'scan', 90)
        scanned = app.wait(lambda s: not s['search_pending'] and s['count'] == 6 and len(s['rows']) == 6, 'six rows')
        assert scanned['scan']['failed'] == 0, scanned['scan']
        report['checks'].append({'step': 'scan', 'names': names(scanned)})

        folder = playlist(app.command('playlist_create', name='Projects', folder=True), 'Projects')
        child = playlist(app.command('playlist_create', name='Ambience', parent_id=folder['id']), 'Ambience')
        other = playlist(app.command('playlist_create', name='Impacts'), 'Impacts')
        assert folder['folder'] and folder['parent_id'] == -1
        assert child['parent_id'] == folder['id'] and not child['folder']
        assert other['parent_id'] == -1
        report['ids'] = {'folder': folder['id'], 'child': child['id'], 'other': other['id']}

        for name in ['Cinematic/war gun.wav', 'Archive/风声.wav']:
            app.command('playlist_add', playlist_id=child['id'], path=str(source / name))
        app.command('playlist_add', playlist_id=other['id'], path=str(source / 'Archive/war rifle.wav'))
        duplicate = app.command('playlist_add', playlist_id=child['id'], path=str(source / 'Cinematic/war gun.wav'))
        assert playlist(duplicate, 'Ambience')['file_count'] == 2, duplicate['playlists']
        report['checks'].append({'step': 'duplicate add', 'file_count': 2})

        rejected = app.command('playlist_set_parent', playlist_id=other['id'], parent_id=child['id'], expect_rejected=True)
        assert playlist(rejected, 'Impacts')['parent_id'] == -1, rejected['playlists']
        report['checks'].append({'step': 'non-folder parent rejected', 'parent_id': -1})

        app.command('playlist_view', playlist_id=child['id'])
        selected = app.wait(lambda s: not s['search_pending'] and s['selected_playlist_id'] == child['id']
                            and s['count'] == 2 and names(s) == ['war gun.wav', '风声.wav'], 'child members')
        assert not selected['playlist_aggregated']
        frame = app.command(capture=True)
        report['checks'].append({'step': 'child members', 'names': names(selected), 'screenshot': f'{frame["id"]:04d}.png'})
        app.command('filter', text='war')
        scoped = app.wait(lambda s: not s['search_pending'] and s['selected_playlist_id'] == child['id']
                          and s['count'] == 1 and names(s) == ['war gun.wav'], 'child scoped keyword')
        report['checks'].append({'step': 'child scoped search', 'names': names(scoped)})

        app.command('filter', text='', folder=str(source / 'Archive'))
        folder_scoped = app.wait(lambda s: not s['search_pending'] and s['selected_playlist_id'] == child['id']
                                 and s['count'] == 1 and names(s) == ['风声.wav'], 'child scoped folder')
        frame = app.command(capture=True)
        report['checks'].append({'step': 'child scoped folder', 'names': names(folder_scoped),
                                 'screenshot': f'{frame["id"]:04d}.png'})
        app.command('filter', folder='')
        app.command('playlist_view', playlist_id=folder['id'])
        aggregate = app.wait(lambda s: not s['search_pending'] and s['selected_playlist_id'] == folder['id']
                             and s['playlist_aggregated'] and s['count'] == 2
                             and names(s) == ['war gun.wav', '风声.wav'], 'folder aggregate')
        frame = app.command(capture=True)
        report['checks'].append({'step': 'folder aggregate', 'names': names(aggregate), 'screenshot': f'{frame["id"]:04d}.png'})
        app.command('playlist_view', playlist_id=other['id'])
        separate = app.wait(lambda s: not s['search_pending'] and s['selected_playlist_id'] == other['id']
                            and s['count'] == 1 and names(s) == ['war rifle.wav'], 'other list stays separate')
        report['checks'].append({'step': 'other list', 'names': names(separate)})
        report['first_exit_code'] = app.close()
        app.retain()

        reopen = output / 'reopen'
        reopen.mkdir()
        app = App(binary, root, reopen)
        restored = app.wait(lambda s: s['database'] == report['library'] and not s['switch_pending']
                            and not s['search_pending'] and s['count'] == 6, 'reopen library', 60)
        for title, original in [('Projects', folder), ('Ambience', child), ('Impacts', other)]:
            current = playlist(restored, title)
            assert current['id'] == original['id'] and current['parent_id'] == original['parent_id'], current
        assert playlist(restored, 'Ambience')['file_count'] == 2
        assert playlist(restored, 'Impacts')['file_count'] == 1
        app.command('playlist_view', playlist_id=folder['id'])
        after = app.wait(lambda s: not s['search_pending'] and s['playlist_aggregated']
                         and s['count'] == 2 and names(s) == ['war gun.wav', '风声.wav'], 'aggregate after reopen')
        frame = app.command(capture=True)
        report['checks'].append({'step': 'reopen aggregate', 'names': names(after),
                                 'screenshot': 'reopen/' + f'{frame["id"]:04d}.png'})
        assert all((source / name).is_file() for name in ['Cinematic/war gun.wav', 'Archive/风声.wav', 'Archive/war rifle.wav'])
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
