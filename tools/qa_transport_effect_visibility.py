"""Exercise transport visibility in the isolated app on a private desktop."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import struct
import time
import traceback
import wave

from PIL import Image
from qa_fresh_result_driver import FreshResultDriver
from qa_required_library_setup import PrivateDesktop, capture, wait_state


CONTROLS = ('transportMono', 'transportReverse', 'transportPan',
            'transportSoundstage', 'transportEQ')
THEMES = ((0, 'classic-dark'), (1, 'aurora'), (2, 'navy'),
          (3, 'sea-glass'), (10, 'mist-pink'))


def fixture_audio(path: Path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), 'wb') as f:
        f.setparams((2, 2, 48000, 0, 'NONE', 'not compressed'))
        frame = b''.join(struct.pack('<hh',
            int(9000 * math.sin(i * 2 * math.pi * 440 / 48000)),
            int(7000 * math.sin(i * 2 * math.pi * 660 / 48000)))
            for i in range(48000))
        f.writeframes(frame * 3)


def run(args):
    import win32con
    import win32event
    import win32gui
    import win32process

    exe, root, out = (args.exe.resolve(), args.isolation_root.resolve(),
                      args.output.resolve())
    assert exe.is_file() and root.is_dir()
    out.mkdir(parents=True, exist_ok=False)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    driver = FreshResultDriver(runtime, out)
    desktop = PrivateDesktop()
    fixture = root / 'fixture' / 'stereo.wav'
    fixture_audio(fixture)
    report = dict(exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  started_at=datetime.now(timezone.utc).isoformat(), checks=[], shots=[])
    process = None
    identity = None

    def cmd(**payload):
        return driver.command(payload)

    def ui():
        return cmd(cmd='settings_ui')

    def settle():
        first = ui()['frame']
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            value = ui()
            if value['frame'] > first + 2:
                return value
            time.sleep(.04)
        raise AssertionError('No fresh settings frame')

    def item(item_id):
        value = next(i for i in settle()['items'] if i['id'] == item_id)
        a, b, c, d = value['bounds']
        l, t, r, bottom = value['clip']
        assert a >= l-1 and b >= t-1 and c <= r+1 and d <= bottom+1, value
        return value

    def click(item_id):
        a, b, c, d = item(item_id)['bounds']
        ox, oy = win32gui.ClientToScreen(identity['hwnd'], (0, 0))
        for phase in ('hover', 'down', 'up'):
            cmd(cmd='theme_schedule', action='pointer', phase=phase,
                x=(a+c)/2-ox, y=(b+d)/2-oy)
            settle()

    def key(name):
        for down in (True, False):
            cmd(cmd='settings_ui', action='key', key=name, down=down)
            settle()

    def query(value):
        click('search')
        cmd(cmd='settings_ui', action='key', key='Ctrl', down=True)
        settle()
        key('A')
        cmd(cmd='settings_ui', action='key', key='Ctrl', down=False)
        settle()
        cmd(cmd='info_edit_input', action='text', text=value)
        settle()

    def open_settings():
        if not ui()['open']:
            cmd(cmd='ordinary_button_window', module='settings', action='open')
            settle()
        cmd(cmd='settings_ui', action='resize', width=1120, height=800)
        settle()
        click('nav-2')

    def toggle(control):
        query('dsp')
        click('result-' + control)
        click(control + '-control')

    def apply():
        assert ui()['pending'], 'Expected a pending settings change'
        click('apply')
        settle()

    def shot(name, settings=False):
        path = out / (name + '.png')
        capture(driver, path)
        report['shots'].append(str(path))
        if settings:
            value = ui()
            a, b, c, d = value['bounds']
            ox, oy = win32gui.ClientToScreen(identity['hwnd'], (0, 0))
            crop = out / (name + '-settings.png')
            with Image.open(path) as image:
                image.crop(tuple(round(x) for x in
                    (a-ox, b-oy, c-ox, d-oy))).save(crop)
            report['shots'].append(str(crop))

    def record(label, **details):
        report['checks'].append(dict(case=label, **details))
        print(label, flush=True)

    def effect():
        return cmd(cmd='transport_effect_state')

    def export(name):
        path = root / 'exports' / (name + '.wav')
        path.parent.mkdir(parents=True, exist_ok=True)
        cmd(cmd='transport_effect_export', path=str(path))
        assert path.stat().st_size > 44
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def bar_audit(visible):
        first = cmd(cmd='transport_audit')
        deadline = time.monotonic() + 10
        last = first
        while time.monotonic() < deadline:
            last = cmd(cmd='transport_audit')
            ids = {entry['id'] for entry in last['checks']}
            if (last['frame'] > first['current_frame'] + 2 and
                    last['current_frame'] - last['frame'] <= 2 and
                    ('stereo' in ids) == visible and ('bass_mono' in ids) == visible and
                    ('eq.label' in ids) == visible):
                return last
            time.sleep(.05)
        raise AssertionError(('Transport did not draw the expected controls', visible, last))

    def start():
        nonlocal process, identity
        process, pid = desktop.start(exe)
        identity = cmd(cmd='magic_eq_identity')
        assert identity['pid'] == pid and Path(identity['exe_path']).resolve() == exe
        assert Path(identity['isolation_root']).resolve() == root
        report.setdefault('launches', []).append(pid)
        return wait_state(driver, lambda value: value['main_ui_ready'],
                          'App must reach the main UI', 90)

    def stop():
        nonlocal process
        if process is None:
            return
        if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
            cmd(cmd='info_edit_input', action='tray_exit')
        assert win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_OBJECT_0
        assert win32process.GetExitCodeProcess(process) == 0
        process.Close()
        process = None

    try:
        initial = start()
        if not initial['db_path']:
            cmd(cmd='info_edit_input', action='new_db_dialog')
            wait_state(driver, lambda value: value['new_db_dialog_open'] and
                       value['new_db_modal_blocks_background'],
                       'First library dialog must be ready', 20)
            cmd(cmd='info_edit_input', action='new_db_name', text='DSP-Visibility-QA')
            time.sleep(.15)
            cmd(cmd='info_edit_input', action='new_db_create')
            initial = wait_state(driver, lambda value: bool(value['db_path']) and
                not value['new_db_dialog_open'], 'Empty isolated library must open', 50)
        assert Path(initial['db_path']).resolve().is_relative_to(root), initial
        cmd(cmd='load_path', path=str(fixture))
        assert all(effect()['visibility'].values())
        record('fresh profile preserves all five visible controls')

        cmd(cmd='transport_audit_setup', scenario='processing')
        active = effect()
        assert active['requested']['reverse'] and active['effective']['reverse'], active
        assert abs(active['effective']['pan']-.5) < .01, active
        assert abs(active['effective']['centerSense']-.6) < .01, active
        assert abs(active['effective']['bassMonoFreq']-80) < .01, active
        assert active['effective']['eq'], active
        active_hash = export('active')
        record('processing fixture enables reverse, pan, soundstage, BM and EQ', state=active)

        open_settings()
        for control, field in (('transportReverse', 'reverse'),
                               ('transportPan', 'pan'),
                               ('transportSoundstage', 'soundstage'),
                               ('transportEQ', 'eq')):
            toggle(control)
            apply()
            bypassed = effect()
            assert not bypassed['visibility'][field], bypassed
            assert export('only-' + field + '-hidden') != active_hash, field
            toggle(control)
            apply()
            assert effect()['effective']['reverse'], field
            assert export('only-' + field + '-restored') == active_hash, field
        record('reverse, pan, soundstage and EQ each change exported audio alone')
        click('done')

        open_settings()
        for control in CONTROLS:
            toggle(control)
        assert all(effect()['visibility'].values()), 'Draft changed live playback'
        shot('01-pending-hide', settings=True)
        cmd(cmd='settings_ui', action='fail_save_once')
        apply()
        failed = ui()
        assert failed['pending'] and failed['error'] and all(effect()['visibility'].values()), failed
        after_failed = effect()
        assert after_failed['effective'] == active['effective'], (after_failed, active)
        record('failed save restores active effects and keeps the draft')
        apply()
        hidden = effect()
        assert all(hidden['visibility'][field] is False for field in
                   ('mono','reverse','pan','soundstage','eq')), hidden
        assert hidden['requested']['reverse'] and hidden['requested']['eq'], hidden
        assert abs(hidden['requested']['pan']-.5) < .01, hidden
        assert abs(hidden['requested']['centerSense']-.6) < .01, hidden
        assert abs(hidden['requested']['bassMonoFreq']-80) < .01, hidden
        assert all(hidden['effective'][field] is False for field in ('mono','reverse','eq')), hidden
        assert all(abs(hidden['effective'][field]) < .01 for field in
                   ('pan','centerSense','bassMonoFreq')), hidden
        hidden_hash = export('hidden')
        assert hidden_hash != active_hash
        record('hiding five controls bypasses processing and retains parameters',
               state=hidden, active_sha256=active_hash, bypass_sha256=hidden_hash)
        click('done')

        audit = bar_audit(False)
        ids = {entry['id'] for entry in audit['checks']}
        assert not ids.intersection({'stereo','reverse','pan.arc','pan.dot',
                                     'center.outline','bass_mono','eq.label','eq.frequency'}), ids
        assert {'timecode','volume.label','database.label'}.issubset(ids), ids
        shot('02-hidden-bar')
        record('hidden bar removes five groups and keeps core playback controls',
               observed=sorted(ids))

        stop()
        start()
        cmd(cmd='load_path', path=str(fixture))
        restarted = effect()
        assert restarted['visibility'] == hidden['visibility'], restarted
        assert restarted['requested'] == hidden['requested'], (restarted, hidden)
        assert restarted['effective'] == hidden['effective'], (restarted, hidden)
        record('restart retains hidden state and effect settings', state=restarted)

        for theme, label in THEMES:
            cmd(cmd='set_theme', theme=theme)
            time.sleep(.3)
            open_settings()
            query('dsp')
            click('result-transportMono')
            shot('03-' + label + '-settings', settings=True)
            click('done')
            snapshot = bar_audit(False)
            ids = {entry['id'] for entry in snapshot['checks']}
            assert 'stereo' not in ids and 'bass_mono' not in ids and 'eq.label' not in ids
            shot('04-' + label + '-bar')
        record('five themes render the real settings and compact transport bar')

        open_settings()
        for control in CONTROLS:
            toggle(control)
        apply()
        restored = effect()
        assert all(restored['visibility'].values()), restored
        assert restored['requested'] == hidden['requested'], (restored, hidden)
        assert restored['effective']['reverse'] and restored['effective']['eq'], restored
        assert abs(restored['effective']['pan']-.5) < .01, restored
        assert abs(restored['effective']['centerSense']-.6) < .01, restored
        assert abs(restored['effective']['bassMonoFreq']-80) < .01, restored
        restored_hash = export('restored')
        assert restored_hash == active_hash, (active_hash, restored_hash)
        click('done')
        shot('05-restored-bar')
        record('showing again restores parameters and exact exported audio', state=restored,
               restored_sha256=restored_hash)

        cmd(cmd='transport_audit_setup', scenario='mono')
        assert effect()['effective']['mono']
        open_settings()
        toggle('transportMono')
        apply()
        mono_hidden = effect()
        assert mono_hidden['requested']['mono'] and not mono_hidden['effective']['mono'], mono_hidden
        toggle('transportMono')
        apply()
        mono_restored = effect()
        assert mono_restored['requested']['mono'] and mono_restored['effective']['mono'], mono_restored
        record('mono is bypassed when hidden and restored when shown')

        cmd(cmd='transport_audit_setup', scenario='mono-reverse')
        both_active = effect()
        assert both_active['effective']['mono'] and both_active['effective']['reverse'], both_active
        both_hash = export('mono-reverse-active')
        toggle('transportMono')
        apply()
        mono_only_hidden = effect()
        assert not mono_only_hidden['effective']['mono'] and mono_only_hidden['effective']['reverse'], mono_only_hidden
        toggle('transportMono')
        apply()
        assert export('mono-reverse-restored') == both_hash
        record('hiding only mono preserves active reverse and the restored selection')

        toggle('transportMono')
        toggle('transportReverse')
        apply()
        both_hidden = effect()
        assert both_hidden['requested']['mono'] and both_hidden['requested']['reverse'], both_hidden
        assert not both_hidden['effective']['mono'] and not both_hidden['effective']['reverse'], both_hidden
        toggle('transportMono')
        toggle('transportReverse')
        apply()
        assert export('mono-reverse-both-restored') == both_hash
        record('hiding mono and reverse together restores both and the selected audio')

        settings_path = root / 'profile/Local/HarkenSearch/settings.ini'
        assert settings_path.is_file(), settings_path
        imported = root / 'imports/reverse-hidden.ini'
        imported.parent.mkdir(parents=True, exist_ok=True)
        lines = settings_path.read_text(encoding='utf-8').splitlines()
        for setting_key, value in (('showTransportReverse', '0'),
                                   ('reverseVisibilityRestore', '1')):
            lines = [setting_key + '=' + value if line.startswith(setting_key + '=') else line
                     for line in lines]
            assert any(line == setting_key + '=' + value for line in lines), setting_key
        imported.write_text('\n'.join(lines) + '\n', encoding='utf-8')
        cmd(cmd='transport_effect_import', path=str(imported))
        imported_state = effect()
        assert imported_state['requested']['reverse'] and not imported_state['effective']['reverse'], imported_state
        toggle('transportReverse')
        apply()
        assert effect()['effective']['reverse'], effect()
        record('importing a hidden reverse setting bypasses current reverse and restores on show')
        click('done')
        report['passed'] = True
    except Exception:
        report['passed'] = False
        report['error'] = traceback.format_exc()
        raise
    finally:
        report['finished_at'] = datetime.now(timezone.utc).isoformat()
        (out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        try:
            stop()
        finally:
            desktop.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('exe', 'isolation-root', 'output'):
        parser.add_argument('--' + option, type=Path, required=True)
    run(parser.parse_args())
