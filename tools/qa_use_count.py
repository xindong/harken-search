"""Verify the use-count column: real rows, real export, real increment.

Runs the isolated QA package on a private desktop with its own library, so the
user's desktop, library and input queue are untouched.
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

WAV_COUNT = 5


def make_library(folder: Path) -> None:
    folder.mkdir(parents=True, exist_ok=False)
    for index, frequency in enumerate((220, 330, 440, 550, 660), start=1):
        samples = (int(3000 * math.sin(2 * math.pi * frequency * n / 22050))
                   for n in range(6615))
        with wave.open(str(folder / f'use-count-{index}.wav'), 'wb') as audio:
            audio.setnchannels(1)
            audio.setsampwidth(2)
            audio.setframerate(22050)
            audio.writeframes(b''.join(sample.to_bytes(2, 'little', signed=True)
                                       for sample in samples))


def main(args: argparse.Namespace) -> int:
    import win32con
    import win32event
    import win32process

    exe, root, output = (args.exe.resolve(), args.isolation_root.resolve(),
                         args.output.resolve())
    if not exe.is_file() or root.exists() or output.exists():
        raise ValueError('Expected an existing EXE and new isolation/output directories')
    root.mkdir(parents=True)
    output.mkdir(parents=True)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True)
    library = root / 'library'
    export_dir = root / 'export-out'
    make_library(library)
    export_dir.mkdir(parents=True, exist_ok=False)

    driver = Driver(runtime, output)
    desktop = PrivateDesktop()
    report = {
        'exe': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
        'isolation_root': str(root), 'started_at': datetime.now(timezone.utc).isoformat(),
        'checks': [], 'screenshots': [],
    }
    process = None
    identity = None

    def command(**payload):
        result = driver.command(payload)
        assert result.get('ok'), result
        return result

    def rows():
        observed = command(cmd='get_state')
        return observed

    def row(index):
        return command(cmd='get_row', row=index)['item']

    def screenshot(name):
        path = output / (name + '.png')
        capture(driver, path)
        report['screenshots'].append(str(path))

    def finish(code: int) -> int:
        nonlocal process
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
            process = None
        (output / 'report.json').write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        return code

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
        # 首次隔离启动要建目录、字体和欢迎页，命令通道可能晚于窗口几十秒才就绪。
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
        state = wait_state(driver, lambda value: value['main_ui_ready'], 'native startup', 60)

        # Fresh library, real WAV files.
        wait_state(driver, lambda value: value['new_db_dialog_open'], 'new library dialog', 15)
        command(cmd='info_edit_input', action='new_db_name', text='UseCountQA')
        time.sleep(.15)
        command(cmd='info_edit_input', action='new_db_create')
        state = wait_state(driver, lambda value: bool(value['db_path']) and
                           not value['new_db_dialog_open'], 'created library', 30)
        assert Path(state['db_path']).resolve().is_relative_to(root), state['db_path']
        scan = command(cmd='scan_folder', path=str(library), recursive=False,
                       cache_waveforms=False, build_similarity_index=False)
        report['scan_start'] = scan
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            status = command(cmd='scan_status')
            report['scan_status'] = status
            if not status.get('scanning', False) and status.get('added', 0) >= WAV_COUNT:
                break
            time.sleep(.3)
        state = wait_state(driver, lambda value: value.get('total_count') == WAV_COUNT,
                           f'{WAV_COUNT} WAV files scanned', 60)

        # Every row starts at zero and the field is exposed to the harness.
        initial = [row(i) for i in range(WAV_COUNT)]
        assert all(item['use_count'] == 0 for item in initial), initial
        report['checks'].append({'case': 'initial use counts are zero',
                                 'rows': [i['use_count'] for i in initial]})
        screenshot('01-use-count-column-zero')

        # Export two rows through the same dialog the user uses.
        command(cmd='select_rows', rows='0,1')
        command(cmd='export_begin', rows='0,1')
        command(cmd='set_export_options', dest_folder=str(export_dir),
                folder_strategy=0, existing_behavior=2, use_source_format=True)
        command(cmd='start_export')
        deadline = time.monotonic() + 60
        completed = 0
        while time.monotonic() < deadline:
            observed = command(cmd='get_state')
            transfer = observed.get('transfer') or {}
            completed = transfer.get('completed_count', 0) or 0
            if completed >= 2 or not transfer.get('in_progress', False):
                break
            time.sleep(.2)
        exported = sorted(p.name for p in export_dir.rglob('*.wav'))
        report['checks'].append({'case': 'exported files', 'files': exported,
                                 'export_completed': completed})
        assert len(exported) == 2, exported

        time.sleep(1.0)
        after_export = [row(i) for i in range(WAV_COUNT)]
        counts_after = [item['use_count'] for item in after_export]
        report['checks'].append({'case': 'use counts after one export',
                                 'rows': counts_after,
                                 'names': [item['name'] for item in after_export]})
        assert counts_after.count(1) == 2, counts_after
        assert counts_after.count(0) == WAV_COUNT - 2, counts_after
        screenshot('02-use-count-after-export')

        # 点表头按使用次数排序：两次点击应给出两个方向。
        click_state = command(cmd='get_state')
        left, top, right, bottom = click_state['use_count_header']
        assert right - left > 4 and bottom - top > 4, click_state['use_count_header']
        center = ((left + right) / 2.0, (top + bottom) / 2.0)
        orders = []
        command(cmd='focus_window')
        for attempt in range(3):
            for phase in ('hover', 'down', 'up'):
                command(cmd='theme_schedule', action='pointer', phase=phase,
                        x=center[0], y=center[1])
                time.sleep(.35)
            time.sleep(1.0)
            order = [row(i)['use_count'] for i in range(WAV_COUNT)]
            if not orders or order != orders[-1]:
                orders.append(order)
            if len(orders) >= 2:
                break
        report['checks'].append({'case': 'orders after header clicks', 'orders': orders,
                                 'header_rect': click_state['use_count_header'],
                                 'sample': click_state.get('drawn_use_count_sample'),
                                 'probe': click_state.get('use_count_header')})
        assert len(orders) >= 2, orders

        def monotonic(values):
            return all(a <= b for a, b in zip(values, values[1:])) or                 all(a >= b for a, b in zip(values, values[1:]))
        assert all(monotonic(values) for values in orders), orders
        assert monotonic(orders[0]) != monotonic(orders[1]) or orders[0] != orders[1], orders
        screenshot('04-sorted-by-use-count')

        # Reload proves the counter lives in the library, not in memory.
        command(cmd='info_edit_input', action='exit')
        assert win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_OBJECT_0
        process.Close()
        process = None
        process, pid = desktop.start(exe, environment_overrides={
            'LOCALAPPDATA': str(root / 'profile/Local'),
            'APPDATA': str(root / 'profile/Roaming'),
            'AURA_RUNTIME_ROOT': str(runtime),
            'TEMP': str(root / 'temp'), 'TMP': str(root / 'temp'),
        })
        wait_state(driver, lambda value: value['main_ui_ready'], 'restart', 60)
        wait_state(driver, lambda value: value['total_count'] == WAV_COUNT,
                   'library reopened', 60)
        persisted = [row(i) for i in range(WAV_COUNT)]
        persisted_counts = [item['use_count'] for item in persisted]
        report['checks'].append({'case': 'use counts after restart',
                                 'rows': persisted_counts})
        assert persisted_counts.count(1) == 2, persisted_counts
        screenshot('03-use-count-after-restart')
        report['status'] = 'passed'
        return finish(0)
    except Exception as error:  # noqa: BLE001 - report the real failure
        report['status'] = 'failed'
        report['error'] = f'{type(error).__name__}: {error}'
        return finish(1)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--isolation-root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    sys.exit(main(parser.parse_args()))
