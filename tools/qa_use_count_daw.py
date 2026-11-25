"""Verify the use-count column increments after a real REAPER hand-off.

Starts a private-desktop REAPER (portable install with the Harken bridge) and
the isolated Harken package, then sends one selected file through the same
SendToDAW path the right-click menu uses. The user's desktop is untouched.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import sys
import time
import wave

from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_transport_themes import Driver

WAV_COUNT = 3
SETTINGS = """[settings]
selectedDAWIndex=0
dawTargetCount=1
dawTarget0_name=REAPER
dawTarget0_exePath={reaper}
dawTarget0_processName=reaper.exe
dawTarget0_mode=3
dawTarget0_httpPort=8080
dawTarget0_enabled=1
dawTarget0_fallback=0
dawTarget0_autoDetect=1
"""


def make_library(folder: Path) -> None:
    folder.mkdir(parents=True, exist_ok=False)
    for index, frequency in enumerate((220, 330, 440), start=1):
        samples = (int(3000 * math.sin(2 * math.pi * frequency * n / 22050))
                   for n in range(6615))
        with wave.open(str(folder / f'daw-use-{index}.wav'), 'wb') as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(22050)
            audio.writeframes(b''.join(sample.to_bytes(2, 'little', signed=True)
                                       for sample in samples))


def main(args: argparse.Namespace) -> int:
    import win32con
    import win32event
    import win32process

    exe, root, output = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    reaper = args.reaper_exe.resolve()
    if not exe.is_file() or not reaper.is_file() or root.exists() or output.exists():
        raise ValueError('Need existing EXEs and new isolation/output directories')
    root.mkdir(parents=True)
    output.mkdir(parents=True)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True)
    library = root / 'library'
    make_library(library)
    settings_dir = root / 'profile/Local/HarkenSearch'
    settings_dir.mkdir(parents=True, exist_ok=True)
    (settings_dir / 'settings.ini').write_text(
        SETTINGS.format(reaper=str(reaper)), encoding='utf-8')

    driver = Driver(runtime, output)
    desktop = PrivateDesktop()
    report = {'exe': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'reaper': str(reaper),
              'isolation_root': str(root),
              'started_at': datetime.now(timezone.utc).isoformat(),
              'checks': [], 'screenshots': []}
    process = reaper_process = None

    def command(**payload):
        result = driver.command(payload)
        assert result.get('ok'), result
        return result

    def row(index):
        return command(cmd='get_row', row=index)['item']

    def stop(proc, grace=15000):
        if proc is None:
            return
        if win32event.WaitForSingleObject(proc, 0) == win32con.WAIT_TIMEOUT:
            win32process.TerminateProcess(proc, 1)
            win32event.WaitForSingleObject(proc, 5000)
        proc.Close()

    try:
        reaper_process, reaper_pid = desktop.start(reaper, extra=('-new',),
                                                  environment_overrides={
                                                      'TEMP': str(root / 'temp'),
                                                      'TMP': str(root / 'temp')})
        report['reaper_pid'] = reaper_pid
        time.sleep(12.0)  # REAPER 载入 bridge 插件

        process, pid = desktop.start(exe, environment_overrides={
            'LOCALAPPDATA': str(root / 'profile/Local'),
            'APPDATA': str(root / 'profile/Roaming'),
            'AURA_RUNTIME_ROOT': str(runtime),
            'TEMP': str(root / 'temp'), 'TMP': str(root / 'temp'),
        })
        report['startup_pid'] = pid
        if win32event.WaitForSingleObject(process, 1000) == win32con.WAIT_OBJECT_0:
            raise RuntimeError('test package exited during startup')
        deadline = time.monotonic() + 120
        while True:
            try:
                identity = command(cmd='magic_eq_identity')
                break
            except TimeoutError:
                stale = runtime / 'qa_cmd.json'
                if stale.exists():
                    stale.unlink()
                if time.monotonic() > deadline:
                    raise
                time.sleep(2.0)
        assert Path(identity['isolation_root']).resolve() == root, identity
        wait_state(driver, lambda value: value['main_ui_ready'], 'native startup', 60)

        wait_state(driver, lambda value: value['new_db_dialog_open'], 'new library dialog', 15)
        command(cmd='info_edit_input', action='new_db_name', text='UseCountDawQA')
        time.sleep(.15)
        command(cmd='info_edit_input', action='new_db_create')
        state = wait_state(driver, lambda value: bool(value['db_path']) and
                           not value['new_db_dialog_open'], 'created library', 30)
        command(cmd='scan_folder', path=str(library), recursive=False,
                cache_waveforms=False, build_similarity_index=False)
        wait_state(driver, lambda value: value.get('total_count') == WAV_COUNT,
                   f'{WAV_COUNT} WAV files scanned', 120)

        before = [row(i)['use_count'] for i in range(WAV_COUNT)]
        report['checks'].append({'case': 'before send', 'rows': before})
        assert before == [0] * WAV_COUNT, before

        command(cmd='select_rows', rows='0')
        command(cmd='send_to_daw', index=0)
        deadline = time.monotonic() + 90
        outcome = None
        while time.monotonic() < deadline:
            feedback = command(cmd='send_feedback')
            outcome = feedback['outcome']
            if outcome not in (1,):
                break
            time.sleep(.4)
        report['checks'].append({'case': 'send outcome', 'outcome': outcome,
                                 'feedback': feedback})
        time.sleep(1.5)
        after = [row(i)['use_count'] for i in range(WAV_COUNT)]
        report['checks'].append({'case': 'after send', 'rows': after,
                                 'outcome': outcome})
        screenshot = output / '01-after-reaper-send.png'
        capture(driver, screenshot)
        report['screenshots'].append(str(screenshot))
        report['status'] = 'passed' if (outcome == 2 and after[0] == 1) else 'failed'
        return 0 if report['status'] == 'passed' else 1
    except Exception as error:  # noqa: BLE001
        report['status'] = 'failed'
        report['error'] = f'{type(error).__name__}: {error}'
        return 1
    finally:
        time.sleep(.5)
        stop(process)
        stop(reaper_process)
        (output / 'report.json').write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--reaper-exe', required=True, type=Path)
    parser.add_argument('--isolation-root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    sys.exit(main(parser.parse_args()))
