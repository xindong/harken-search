"""Exercise the real Settings clear flow in this card's isolated QA package."""
from pathlib import Path
import argparse
import hashlib
import json
import math
import sqlite3
import struct
import time
import traceback
import wave

import win32con
import win32event
import win32gui
import win32process
from PIL import Image

from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_scan_speed import contains_root_literal
from qa_transport_themes import Driver


def tone(path, frequency):
    path.parent.mkdir(parents=True, exist_ok=True)
    samples = (struct.pack('<h', round(9000 * math.sin(2 * math.pi * frequency * n / 22050)))
               for n in range(2 * 22050))
    with wave.open(str(path), 'wb') as out:
        out.setparams((1, 2, 22050, 0, 'NONE', 'not compressed'))
        out.writeframes(b''.join(samples))


def database_snapshot(path):
    with sqlite3.connect(path.as_uri() + '?mode=ro', uri=True, timeout=10) as db:
        db.execute('PRAGMA query_only=ON')
        db.execute('BEGIN')
        digest = hashlib.sha256()
        for row in db.execute('SELECT * FROM audio_files ORDER BY id'):
            for value in row:
                value = value if isinstance(value, bytes) else str(value).encode('utf-8')
                digest.update(len(value).to_bytes(8, 'little'))
                digest.update(value)
        tables = {row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        count = lambda name: db.execute(f'SELECT COUNT(*) FROM {name}').fetchone()[0] if name in tables else 0
        return {'files': count('audio_files'), 'audio_sha256': digest.hexdigest(),
                'analysis': count('harken_similarity_v2'),
                'segments': count('harken_similarity_segments_v2'),
                'identity': db.execute('SELECT identity FROM harken_similarity_identity WHERE singleton=1').fetchone()[0]
                if 'harken_similarity_identity' in tables else None}


def run(args):
    exe, root, output = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    assert output.is_relative_to(root.parent) and not output.is_relative_to(root)
    assert exe.is_file() and contains_root_literal(exe, root), 'This QA EXE must be pinned to this isolation root'
    output.mkdir(parents=True, exist_ok=False)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    database = root / 'profile/Local/HarkenSearch/Database/ClearQA/main.db'
    database.parent.mkdir(parents=True, exist_ok=True)
    if not database.exists():
        sqlite3.connect(database).close()
    settings = root / 'profile/Local/HarkenSearch/settings.ini'
    if settings.exists():
        assert str(database) in settings.read_text(encoding='utf-8-sig'), 'Unexpected QA profile database'
    else:
        settings.write_text('showWelcomeOnStartup=0\ncloseButtonAction=2\ncurrentDbPath=' + str(database) + '\n', encoding='utf-8')
    source = root / 'synthetic-audio'
    for index, hz in enumerate((440, 660, 880), 1):
        tone(source / f'clear-tone-{index}.wav', hz)
    driver = Driver(runtime, output)
    desktop = PrivateDesktop()
    process = None
    identity = None
    verified = False
    report = {'exe': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'root': str(root), 'database': str(database), 'passed': False, 'checks': []}

    def save():
        (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    def command(**request):
        return driver.command(request, timeout=60)

    def until(probe, predicate, label, timeout=90):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            last = probe()
            if predicate(last):
                return last
            time.sleep(.1)
        raise TimeoutError(f'{label}: {last}')

    def launch():
        nonlocal process, identity, verified
        verified = False
        process, pid = desktop.start(exe)
        identity = command(cmd='magic_eq_identity')
        assert identity['pid'] == pid and Path(identity['exe_path']).resolve() == exe
        assert Path(identity['isolation_root']).resolve() == root
        verified = True
        ready = wait_state(driver, lambda s:s['main_ui_ready'] and not s['db_switch_in_progress'], 'ready', 90)
        assert Path(ready['db_path']).resolve() == database, ready
        win32gui.SetWindowPos(identity['hwnd'], 0, 40, 40, 1480, 940,
                             win32con.SWP_NOACTIVATE | win32con.SWP_NOZORDER)

    def stop():
        nonlocal process
        if process is None:
            return
        if verified:
            command(cmd='info_edit_input', action='tray_exit')
            assert win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_OBJECT_0
            assert win32process.GetExitCodeProcess(process) == 0
        elif win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
            win32process.TerminateProcess(process, 1)
        process.Close()
        process = None

    def settings_state():
        return command(cmd='settings_ui')

    def settled():
        first = settings_state()['frame']
        return until(settings_state, lambda s:s['frame'] > first + 2, 'fresh settings frame', 8)

    def item(key):
        item = next(i for i in settled()['items'] if i['id'] == key)
        a, b, c, d = item['bounds']
        l, t, r, bottom = item['clip']
        assert a >= l - 1 and b >= t - 1 and c <= r + 1 and d <= bottom + 1, item
        assert not item['disabled'], item
        return item

    def pointer(x, y):
        ox, oy = win32gui.ClientToScreen(identity['hwnd'], (0, 0))
        for phase in ('hover', 'down', 'up'):
            command(cmd='theme_schedule', action='pointer', phase=phase, x=x-ox, y=y-oy)
            settled()

    def click(key):
        a, b, c, d = item(key)['bounds']
        pointer((a+c)/2, (b+d)/2)

    def key(name):
        for down in (True, False):
            command(cmd='settings_ui', action='key', key=name, down=down)
            settled()

    def query(value):
        click('search')
        command(cmd='settings_ui', action='key', key='Ctrl', down=True)
        settled(); key('A')
        command(cmd='settings_ui', action='key', key='Ctrl', down=False)
        settled()
        command(cmd='info_edit_input', action='text', text=value)
        settled()

    def shot(name):
        path = output / (name + '.png')
        capture(driver, path)
        popup = next((p for p in settings_state()['color_popups'] if 'SettingsTool' in p['name']), None)
        if popup:
            a, b, c, d = popup['bounds']
            ox, oy = win32gui.ClientToScreen(identity['hwnd'], (0, 0))
            with Image.open(path) as frame:
                frame.crop(tuple(round(v) for v in (a-ox, b-oy, c-ox, d-oy))).save(output / (name + '-tool.png'))

    def open_tool():
        command(cmd='ordinary_button_window', module='settings', action='open')
        settled()
        command(cmd='settings_ui', action='resize', width=1120, height=800)
        settled()
        query('maintenance')
        click('result-maintenance')
        click('maintenance')
        assert settled()['tool'] == 'maintenance'

    def analyze():
        command(cmd='similar_index', action='open', floating=True, width=900, height=620, language=0)
        current = until(lambda:command(cmd='similar_index', action='state'),
                        lambda s:s['total'] == 3 and 'similar.index.start' in s['controls'], 'analysis panel')
        generation = current['batch_start_generation']
        command(cmd='similar_index', action='start')
        return until(lambda:command(cmd='similar_index', action='state'),
                     lambda s:s['batch_start_generation'] > generation and not s['running'] and s['batch_total'] == 3,
                     'analysis finished', 180)

    try:
        launch()
        if database_snapshot(database)['files'] == 0:
            started = command(cmd='scan_folder', path=str(source), recursive=True,
                              scan_strategy='fast', build_similarity_index=False)
            assert started['started'], started
            scan = until(lambda:command(cmd='scan_status'),
                         lambda s:not s['scanning'] and not s['scan_completion_pending'], 'synthetic scan')
            assert scan['errors'] == 0, scan
            command(cmd='scan_acknowledge_complete')
            until(lambda:command(cmd='info_page_state'), lambda s:'info.qaScanDone' in s['zones'], 'scan close')
            for phase in ('hover', 'down', 'up'):
                command(cmd='info_page_state')
                command(cmd='info_page_pointer', key='info.qaScanDone', phase=phase)
                time.sleep(.1)
        assert database_snapshot(database)['files'] == 3
        if database_snapshot(database)['analysis'] != 3:
            result = analyze()
            assert result['analyzed'] == 3 and result['batch_summary']['failed'] == 0, result
        before = database_snapshot(database)
        assert before['files'] == before['analysis'] == 3, before
        report['before'] = before
        open_tool()
        shot('01-clear-entry')
        if args.red:
            item('similar-clear-request')
            report['passed'] = True
            return
        click('similar-clear-request')
        item('similar-clear-confirm')
        shot('02-confirmation')
        assert database_snapshot(database) == before, 'Request changed the database before confirmation'
        click('similar-clear-cancel')
        assert database_snapshot(database) == before, 'Cancel changed the database'
        report['checks'].append('Request and cancel preserve the complete QA library')
        click('similar-clear-request')
        click('similar-clear-confirm')
        cleared = until(lambda:database_snapshot(database), lambda s:s['analysis'] == 0,
                        'analysis cleared')
        assert cleared['segments'] == 0 and cleared['files'] == before['files']
        assert cleared['audio_sha256'] == before['audio_sha256']
        assert cleared['identity'] == before['identity']
        report['cleared'] = cleared
        report['checks'].append('Confirmed clear removes only current-library analysis and segments')
        shot('03-cleared')
        click('tool-close')
        click('done')
        current = until(lambda:command(cmd='similar_index', action='state'),
                        lambda s:s['indexed'] == 0 and s['total'] == 3, 'visible zero coverage')
        assert not current['completion_notice'], current
        report['checks'].append('Visible analysis coverage and completion notice reset')
        rebuilt = analyze()
        assert rebuilt['analyzed'] == 3 and rebuilt['batch_summary']['failed'] == 0, rebuilt
        assert database_snapshot(database)['analysis'] == 3
        report['checks'].append('All synthetic audio can be reanalyzed after clearing')
        report['passed'] = True
    except Exception:
        report['error'] = traceback.format_exc()
        raise
    finally:
        save()
        try:
            stop()
        finally:
            desktop.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--red', action='store_true')
    run(parser.parse_args())
