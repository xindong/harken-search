#!/usr/bin/env python3
"""Capture the real Similar Sounds window in an isolated Mac QA library."""
import argparse
import json
from pathlib import Path
import sqlite3
import time

from macos_library_smoke import App, save


def png_dimensions(path):
    data = path.read_bytes()
    assert data.startswith(b'\x89PNG\r\n\x1a\n') and len(data) > 5000, path
    return int.from_bytes(data[16:20], 'big'), int.from_bytes(data[20:24], 'big')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--library-source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root, source = args.root.resolve(), args.library_source.resolve()
    assert source.is_file() and source.is_relative_to(root), 'source must be an isolated library'
    args.output.mkdir(parents=True, exist_ok=False)
    target = root / 'similar-window-libraries' / args.output.name / 'main.db'
    target.parent.mkdir(parents=True, exist_ok=False)
    with sqlite3.connect(source) as original, sqlite3.connect(target) as copy:
        original.backup(copy)

    report = {'status': 'FAIL', 'source': str(source), 'library': str(target),
              'scope': 'real Similar Sounds window, isolated library, screenshot and normal exit'}
    app = App(args.app, root, args.output)
    try:
        app.wait(lambda s: not s['switch_pending'], 'startup mount', 60)
        app.command('open', path=str(target))
        ready = app.wait(lambda s: s['database_open'] and s['database'] == str(target)
                         and not s['switch_pending'] and s['count'] == 7,
                         'copied library visible', 60)
        assert len(ready['rows']) == 7, 'expected seven independent test records'
        sample_index = next(index for index, row in enumerate(ready['rows'])
                            if row['name'] == 'stone-impact.wav')
        sample_path = ready['rows'][sample_index]['path']
        app.command('select', index=sample_index)
        app.wait(lambda s: s['audio']['loaded'] and s['audio']['path'] == sample_path,
                 'sample loaded for similarity reference', 60)
        opened = app.command('open_similar', capture=True)
        assert opened['similar_window'] == {'visible': True, 'floating': True}
        time.sleep(1.0)
        after = app.command(capture=True)
        assert after['similar_window'] == {'visible': True, 'floating': True}
        screenshots = sorted(args.output.glob('*.png'))
        assert len(screenshots) == 2, 'missing real window capture'
        sizes = [png_dimensions(path) for path in screenshots]
        assert all(width >= 400 and height >= 300 for width, height in sizes), sizes
        report.update(status='PASS', visible=True, count=ready['count'],
                      reference=sample_path,
                      screenshots=[str(path) for path in screenshots], sizes=sizes,
                      exit_code=app.close())
    finally:
        if app.process.poll() is None:
            app.close()
        app.retain()
        save(args.output / 'report.json', report)
    print(json.dumps(report, ensure_ascii=False))


if __name__ == '__main__':
    main()
