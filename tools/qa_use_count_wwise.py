"""Verify the use-count column increments after a real Wwise (WAAPI) transfer.

Expects an already running WwiseConsole waapi-server for the test project.
Runs the isolated package on a private desktop with its own library.
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
BACKSLASH = chr(92)
CONFIG = {
    "version": 2,
    "activePresetName": "",
    "presets": [],
    "active": {
        "version": 1,
        "projectPath": "",
        "waapiHost": "127.0.0.1",
        "waapiPort": 8093,
        "originalsSubfolder": "UseCountQA",
        "importDestination": BACKSLASH + BACKSLASH + "Actor-Mixer Hierarchy" + BACKSLASH + BACKSLASH + "Default Work Unit",
        "conflictPolicy": "CreateNew",
        "templateApply": "NewOnly",
        "userVars": {},
        "structure": [{
            "type": "SoundSFX",
            "nameTemplate": "$filename_base",
            "propertyTemplatePath": "",
            "useTemplate": False,
        }],
    },
}


def make_library(folder: Path) -> None:
    folder.mkdir(parents=True, exist_ok=False)
    for index, frequency in enumerate((220, 330, 440), start=1):
        samples = (int(3000 * math.sin(2 * math.pi * frequency * n / 22050))
                   for n in range(6615))
        with wave.open(str(folder / f'wwise-use-{index}.wav'), 'wb') as audio:
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
    if not exe.is_file() or root.exists() or output.exists():
        raise ValueError('Need an existing EXE and new isolation/output directories')
    root.mkdir(parents=True)
    output.mkdir(parents=True)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True)
    library = root / 'library'
    make_library(library)
    settings_dir = root / 'profile/Local/HarkenSearch'
    settings_dir.mkdir(parents=True, exist_ok=True)
    config = json.loads(json.dumps(CONFIG))
    config['active']['projectPath'] = str(args.project.resolve())
    config['active']['waapiPort'] = args.waapi_port
    (settings_dir / 'wwise_transfer.json').write_text(
        json.dumps(config, ensure_ascii=False, indent=2), encoding='utf-8')

    driver = Driver(runtime, output)
    desktop = PrivateDesktop()
    report = {'exe': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'project': str(args.project), 'waapi_port': args.waapi_port,
              'isolation_root': str(root),
              'started_at': datetime.now(timezone.utc).isoformat(),
              'checks': [], 'screenshots': []}
    process = None

    def command(**payload):
        result = driver.command(payload)
        assert result.get('ok'), result
        return result

    def row(index):
        return command(cmd='get_row', row=index)['item']

    try:
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
        command(cmd='info_edit_input', action='new_db_name', text='UseCountWwiseQA')
        time.sleep(.15)
        command(cmd='info_edit_input', action='new_db_create')
        wait_state(driver, lambda value: bool(value['db_path']) and
                   not value['new_db_dialog_open'], 'created library', 30)
        command(cmd='scan_folder', path=str(library), recursive=False,
                cache_waveforms=False, build_similarity_index=False)
        wait_state(driver, lambda value: value.get('total_count') == WAV_COUNT,
                   f'{WAV_COUNT} WAV files scanned', 120)

        before = [row(i)['use_count'] for i in range(WAV_COUNT)]
        report['checks'].append({'case': 'before transfer', 'rows': before})
        assert before == [0] * WAV_COUNT, before

        command(cmd='select_rows', rows='0,1')
        staged = command(cmd='wwise_qa', action='stage')
        configured = command(cmd='wwise_qa', action='configure',
                             port=args.waapi_port, project=str(args.project.resolve()))
        report['checks'].append({'case': 'configure', 'port': configured.get('port'),
                                 'project': configured.get('project'),
                                 'layers': configured.get('structure_layers')})
        connect = command(cmd='wwise_qa', action='connect')
        report['checks'].append({'case': 'staged', 'staged': staged.get('staged')})
        report['checks'].append({'case': 'connect',
                                 'connected': connect.get('connected'),
                                 'message': connect.get('message')})
        assert connect.get('connected'), connect

        command(cmd='wwise_qa', action='transfer')
        deadline = time.monotonic() + 180
        status = {}
        while time.monotonic() < deadline:
            status = command(cmd='wwise_qa', action='status')
            if status.get('has_result') and not status.get('active'):
                break
            time.sleep(1.0)
        report['checks'].append({'case': 'transfer', 'status': status})
        time.sleep(2.0)
        after = [row(i)['use_count'] for i in range(WAV_COUNT)]
        report['checks'].append({'case': 'after transfer', 'rows': after,
                                 'imported': status.get('imported'),
                                 'failed_items': status.get('failed_items'),
                                 'aggregate_error': status.get('aggregate_error')})
        command(cmd='wwise_qa', action='close')
        time.sleep(1.5)
        closed = [row(i)['use_count'] for i in range(WAV_COUNT)]
        report['checks'].append({'case': 'after panel close', 'rows': closed})
        shot = output / '01-after-wwise-transfer.png'
        capture(driver, shot)
        report['screenshots'].append(str(shot))
        ok = (status.get('failed') is False
              and (status.get('imported') or 0) >= 2
              and after == [1, 1, 0])
        report['status'] = 'passed' if ok else 'failed'
        return 0 if ok else 1
    except Exception as error:  # noqa: BLE001
        report['status'] = 'failed'
        report['error'] = f'{type(error).__name__}: {error}'
        return 1
    finally:
        if process is not None:
            if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
                try:
                    command(cmd='info_edit_input', action='exit')
                except Exception:
                    pass
                if win32event.WaitForSingleObject(process, 15000) != win32con.WAIT_OBJECT_0:
                    win32process.TerminateProcess(process, 1)
                    report['forced_termination'] = True
            process.Close()
        (output / 'report.json').write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--project', required=True, type=Path)
    parser.add_argument('--waapi-port', type=int, default=8093)
    parser.add_argument('--isolation-root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    sys.exit(main(parser.parse_args()))
