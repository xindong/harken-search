"""Real isolated UI check: one action validates unchanged, changed, and failed audio."""
from pathlib import Path
import argparse
import hashlib
import json
import math
import os
import sqlite3
import struct
import time
import traceback
import wave

from PIL import Image
import win32con
import win32event
import win32process

from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_scan_speed import contains_root_literal
from qa_shared_buttons import audit
from qa_transport_themes import Driver


def check_semantic_coverage(state):
    assert 0 <= state['semantic_indexed'] <= state['indexed'], state


def tone(path, frequency):
    path.parent.mkdir(parents=True, exist_ok=True)
    samples = (struct.pack('<h', round(9000 * math.sin(2 * math.pi * frequency * n / 22050)))
               for n in range(2 * 22050))
    with wave.open(str(path), 'wb') as out:
        out.setparams((1, 2, 22050, 0, 'NONE', 'not compressed'))
        out.writeframes(b''.join(samples))


def run(args):
    exe, root, output = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    assert output.is_relative_to(root.parent) and not output.is_relative_to(root)
    assert exe.is_file() and contains_root_literal(exe, root), 'QA EXE must contain this exact isolation root'
    output.mkdir(parents=True, exist_ok=False)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    db = root / 'profile/Local/HarkenSearch/Database/SingleActionQA/main.db'
    db.parent.mkdir(parents=True, exist_ok=True)
    if not db.exists():
        sqlite3.connect(db).close()
        existing_records = 0
    else:
        with sqlite3.connect(db) as connection:
            existing_records = connection.execute('SELECT COUNT(*) FROM audio_files').fetchone()[0]
        assert existing_records in (0, 3), 'Unexpected existing QA database population'
        if args.reset_fixture and existing_records == 3:
            with sqlite3.connect(db) as connection:
                connection.execute('DELETE FROM harken_similarity_segments_v2')
                connection.execute('DELETE FROM harken_similarity_v2')
    settings = root / 'profile/Local/HarkenSearch/settings.ini'
    if settings.exists():
        assert str(db) in settings.read_text(encoding='utf-8-sig'), 'Refusing an unrelated QA profile'
    else:
        settings.write_text('showWelcomeOnStartup=0\ncloseButtonAction=2\ncurrentDbPath=' + str(db) + '\n', encoding='utf-8')
    source = root / 'synthetic-audio'
    for index, hz in enumerate((440, 660, 880), 1):
        tone(source / f'tone-{index}.wav', hz)

    driver = Driver(runtime, output)
    desktop = PrivateDesktop()
    process = None
    identity = None
    verified = False
    report = {'exe': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'root': str(root), 'database': str(db), 'passed': False, 'checks': []}

    def save():
        (output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    def command(**request):
        return driver.command(request, timeout=60)

    def index():
        return command(cmd='similar_index', action='state')

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
        ready = wait_state(driver, lambda s: s['main_ui_ready'] and not s['db_switch_in_progress'], 'ready', 90)
        assert Path(ready['db_path']).resolve() == db, ready
        return pid

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

    def click_start():
        for phase in ('hover', 'down', 'up'):
            audit(driver, command(cmd='get_state')['theme'])
            bounds = index()['controls']['similar.index.start']
            command(cmd='similar_index', action='pointer', phase=phase,
                    x=(bounds[0] + bounds[2]) / 2, y=(bounds[1] + bounds[3]) / 2)
            time.sleep(.1)

    def screenshot(name, control):
        path = output / (name + '.png')
        capture(driver, path)
        left, top, right, bottom = control
        with Image.open(path) as frame:
            bounds = (max(0, round(left - 42)), max(0, round(top - 208)),
                      min(frame.width, round(right + 42)), min(frame.height, round(bottom + 397)))
            frame.crop(bounds).save(output / (name + '-panel.png'))

    try:
        report['first_pid'] = launch()
        if existing_records == 0:
            started = command(cmd='scan_folder', path=str(source), recursive=True,
                              scan_strategy='fast', build_similarity_index=False)
            assert started['started'], started
            scan = until(lambda: command(cmd='scan_status'),
                         lambda s: not s['scanning'] and not s['scan_completion_pending'], 'scan')
            assert scan['errors'] == 0, scan
            command(cmd='scan_acknowledge_complete')
            until(lambda: command(cmd='info_page_state'),
                  lambda s: 'info.qaScanDone' in s['zones'], 'scan close button')
            for phase in ('hover', 'down', 'up'):
                command(cmd='info_page_state')
                command(cmd='info_page_pointer', key='info.qaScanDone', phase=phase)
                time.sleep(.1)
        command(cmd='similar_index', action='open', floating=True, width=900, height=620, language=0)
        first = until(index, lambda s: s['total'] == 3 and 'similar.index.start' in s['controls'], 'first coverage')
        click_start()
        initial_batch = until(index, lambda s: s['batch_start_generation'] > first['batch_start_generation']
                              and not s['running'] and s['batch_total'] == 3, 'first batch', 180)
        assert initial_batch['analyzed'] == 3 and initial_batch['batch_summary']['failed'] == 0, initial_batch
        report['checks'].append('Initial real batch analyzed all three synthetic files')
        stop()

        with sqlite3.connect(db) as connection:
            files = {Path(path).name: file_id for file_id, path in
                     connection.execute('SELECT id,file_path FROM audio_files')}
            assert set(files) == {'tone-1.wav', 'tone-2.wav', 'tone-3.wav'}, files
            original = {file_id: fingerprint for file_id, fingerprint in
                        connection.execute('SELECT file_id,fingerprint FROM harken_similarity_v2')}
            file_id = files['tone-3.wav']
            connection.execute('DELETE FROM harken_similarity_segments_v2 WHERE file_id=?', (file_id,))
            connection.execute('DELETE FROM harken_similarity_v2 WHERE file_id=?', (file_id,))
        modified = source / 'tone-2.wav'
        timestamp = modified.stat().st_mtime_ns
        tone(modified, 990)
        os.utime(modified, ns=(timestamp, timestamp))
        missing = source / 'tone-3.wav'
        missing.rename(missing.with_name(missing.name + '.' + output.name + '.offline'))
        report['fixture'] = {'missing_analysis_id': file_id, 'changed_id': files['tone-2.wav'],
                             'unchanged_id': files['tone-1.wav']}

        report['second_pid'] = launch()
        command(cmd='similar_index', action='open', floating=True, width=900, height=620, language=0)
        command(cmd='set_theme', theme=2)
        before = until(index, lambda s: s['total'] == 3 and s['indexed'] == 2
                       and 'similar.index.start' in s['controls'], 'partial coverage')
        assert 'similar.index.recheck' not in before['controls'], before['controls']
        time.sleep(2.5)  # asynchronous read-only counter publishes to the visible panel
        screenshot('single-action-before', before['controls']['similar.index.start'])
        report['before'] = before
        check_semantic_coverage(before)
        click_start()
        resumed = until(index, lambda s: s['batch_start_generation'] > before['batch_start_generation']
                        and not s['running'] and s['batch_total'] == 3, 'full validation batch', 180)
        assert resumed['progress'] == 3 and resumed['analyzed'] == 1 and resumed['reused'] == 1, resumed
        assert resumed['batch_summary']['failed'] == 1, resumed
        with sqlite3.connect(db) as connection:
            after = {file_id: fingerprint for file_id, fingerprint in
                     connection.execute('SELECT file_id,fingerprint FROM harken_similarity_v2')}
        assert after[files['tone-1.wav']] == original[files['tone-1.wav']]
        assert after[files['tone-2.wav']] != original[files['tone-2.wav']]
        assert files['tone-3.wav'] not in after
        screenshot('single-action-after', resumed['controls']['similar.index.start'])
        report['resumed'] = resumed
        report['checks'].append('One visible action checked all three: reused unchanged, analyzed changed, reported unreadable')
        report['passed'] = True
        save()
    except Exception:
        report['error'] = traceback.format_exc()
        save()
        raise
    finally:
        try:
            stop()
        finally:
            desktop.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reset-fixture', action='store_true', help='reset only this pinned synthetic QA database')
    run(parser.parse_args())
