"""Capture a real scan database-write failure on this card's private desktop.

The newly created QA database has a trigger that aborts inserts during the scan.
No user library or desktop input is used. Exit 0 means the real failure and
screenshots were captured, not that all failed files were classified correctly.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sqlite3
import sys
import traceback
import uuid

from qa_required_library_setup import PrivateDesktop, capture
from qa_scan_dialog_simple import Smoke, inside, make_wavs, path_key, require, save_json, sha256, utc_now
from qa_transport_themes import Driver


CARD_ROOT = Path('D:/Harken-QA/BUG-20261101-SCAN-DBWRITE-CLASSIFY')
ABORT_MESSAGE = 'Scan aborted after database write failure; queued files were not imported.'
INPUT_COUNT = 500


def run(args):
    import win32con
    import win32event
    import win32process

    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    require(exe.is_file() and inside(exe, CARD_ROOT) and inside(root, CARD_ROOT)
            and inside(out, CARD_ROOT), '包、隔离目录和输出必须属于本卡 QA 目录')
    require(not out.exists(), '输出目录已存在，拒绝覆盖旧证据')
    runtime = root / 'profile/runtime'
    require(not (runtime / 'qa_cmd.json').exists()
            and not (runtime / 'qa_scan_dialog_simple.lock').exists(), '本卡隔离环境有其他 QA 命令')
    out.mkdir(parents=True)
    runtime.mkdir(parents=True, exist_ok=True)
    report = {'status': 'running', 'started_at': utc_now(), 'exe': str(exe),
              'exe_sha256': sha256(exe), 'isolation_root': str(root),
              'human_visual_acceptance': '未验收',
              'not_covered': ['没有模拟真实磁盘损坏',
                              '未复现队列残留消息；本次只验证隔离库写入触发器中止扫描的路径']}
    lock = runtime / 'qa_scan_dialog_simple.lock'  # shared by both scan QA helpers
    owner = json.dumps({'pid': os.getpid(), 'output': str(out), 'nonce': uuid.uuid4().hex})
    desktop = process = driver = ui = database = None
    verified = False
    process_exited = True
    failure_fixture_installed = False
    with lock.open('x', encoding='utf-8') as handle:
        lock_identity = os.fstat(handle.fileno())
        handle.write(owner)
        handle.flush()
    try:
        driver = Driver(runtime, out)
        desktop = PrivateDesktop()
        process, pid = desktop.start(exe)
        process_exited = False
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=60)
        require(identity['pid'] == pid and path_key(identity['exe_path']) == path_key(exe)
                and path_key(identity['isolation_root']) == path_key(root), '运行包身份与隔离目录不符')
        verified = True
        report['identity'] = identity
        ui = Smoke(driver, root, out, 120)
        ui.wait(lambda s: s['main_ui_ready'] and not s['db_switch_in_progress'],
                '等待主界面', 60, lambda: driver.command({'cmd': 'get_state'}))
        previous_database = driver.command({'cmd': 'get_state'}).get('db_path')
        database = ui.create_library(always_new=True)
        if previous_database and path_key(database) == path_key(previous_database):
            switched = ui.wait(
                lambda s: s.get('db_path') and path_key(s['db_path']) != path_key(previous_database)
                and not s.get('db_switch_in_progress'),
                '等待新建测试库切换', 30, lambda: driver.command({'cmd': 'get_state'}))
            database = Path(switched['db_path']).resolve()
        require(not previous_database or path_key(database) != path_key(previous_database),
                '新建测试库尚未切换，拒绝修改原库')
        require(inside(database, root / 'profile') and not ui.rows(database),
                '新建测试库不在本卡隔离目录或已经有文件')
        report['database'] = str(database)
        with sqlite3.connect(database, timeout=5) as connection:
            connection.execute("CREATE TRIGGER qa_scan_dbwrite_abort BEFORE INSERT ON audio_files "
                               "BEGIN SELECT RAISE(ABORT, 'qa forced database write failure'); END")
            connection.commit()
            failure_fixture_installed = True
            present = connection.execute("SELECT 1 FROM sqlite_master WHERE type='trigger' "
                                         "AND name='qa_scan_dbwrite_abort'").fetchone()
        require(present is not None, '写入故障夹具未生效')
        report['failure_setup'] = '新建隔离库的 INSERT 触发器强制中止数据库写入'
        folder = root / 'fixtures' / ('dbwrite-' + uuid.uuid4().hex[:10])
        make_wavs(folder, INPUT_COUNT, seconds=1)
        report['input_folder'] = str(folder)
        driver.command({'cmd': 'scan_dialog_ui', 'action': 'theme', 'theme': 0})
        started = driver.command({'cmd': 'scan_folder', 'path': str(folder),
                                  'worker_threads': 1, 'scan_strategy': 'full'})
        require(started.get('started') and path_key(started['path']) == path_key(folder),
                '单线程真实扫描未启动')
        report['scan_start_command'] = started
        state = ui.wait(lambda s: s['surface'] == 'result' and s['phase'] == 'failed'
                        and ui.target(s, 'scan.result.failures', '##ScanResult') is not None,
                        '等待真实数据库写入失败结果', 120)
        status = driver.command({'cmd': 'scan_status'})
        report['result_state'] = state
        report['scan_status'] = status
        require(not state['is_scanning'] and not state['completion_pending']
                and state['succeeded'] == 0 and state['failed'] == INPUT_COUNT,
                '扫描失败计数与整批文件夹具不符')
        report['target_message_seen'] = ABORT_MESSAGE in status['error_details']
        require(status['errors'] == INPUT_COUNT and any(
                message.startswith('Database write failed during scan:')
                for message in status['error_details']), '未走到真实数据库写入失败路径')
        require(not ui.rows(database), '写入失败后仍有文件入库')
        report['result_png'] = ui.shot('dbwrite-result', 'scan.result.failures', '##ScanResult')
        ui.click('scan.result.failures', '##ScanResult')
        report['details_png'] = ui.shot('dbwrite-details', 'scan.failures.back', '##ScanFailureDetails')
        ui.click('scan.failures.error.0', '##ScanFailureDetails')
        report['expanded_details_png'] = ui.shot(
            'dbwrite-details-expanded', 'scan.failures.back', '##ScanFailureDetails')
        report['status'] = 'captured'
    except Exception as error:
        report.update(status='failed', error=str(error), traceback=traceback.format_exc())
        if ui is not None:
            report['last_observed_state'] = ui.last
        if verified:
            try:
                capture(driver, out / 'failure-diagnostic.png')
            except Exception as capture_error:
                report['capture_error'] = str(capture_error)
    finally:
        try:
            if process is not None:
                if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
                    if verified:
                        try:
                            driver.command({'cmd': 'info_edit_input', 'action': 'exit'}, timeout=10)
                        except Exception as error:
                            report['exit_command_error'] = str(error)
                    if win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_TIMEOUT:
                        win32process.TerminateProcess(process, 1)  # only this private process handle
                        report.update(status='failed', forced_private_exit=True)
                require(win32event.WaitForSingleObject(process, 5000) == win32con.WAIT_OBJECT_0,
                        '私有测试进程未确认退出')
                process_exited = True
                report['process_exit_code'] = win32process.GetExitCodeProcess(process)
            if process_exited and failure_fixture_installed:
                with sqlite3.connect(database, timeout=5) as connection:
                    connection.execute('DROP TRIGGER qa_scan_dbwrite_abort')
                    connection.commit()
                    remaining_fixture = connection.execute(
                        "SELECT 1 FROM sqlite_master WHERE type='trigger' "
                        "AND name='qa_scan_dbwrite_abort'").fetchone()
                require(remaining_fixture is None, '故障夹具未从本次测试库移除')
                report['failure_fixture_removed'] = True
            if process is not None:
                require(report['process_exit_code'] == 0, '程序退出码非零')
            require(sha256(exe) == report['exe_sha256'], '测试期间运行包发生变化')
        except Exception as error:
            report.update(status='failed', cleanup_error=str(error))
        finally:
            if process is not None:
                process.Close()
            if desktop is not None:
                desktop.close()
            if process_exited:
                require(os.path.samestat(lock_identity, lock.stat(follow_symlinks=False))
                        and lock.read_text(encoding='utf-8') == owner, '锁归属变化，拒绝清理')
                lock.unlink()
            else:
                report['lock_retained'] = str(lock)
            report['finished_at'] = utc_now()
            save_json(out / 'report.json', report)
    print(json.dumps({'status': report['status'], 'report': str(out / 'report.json')}, ensure_ascii=False), flush=True)
    return 0 if report['status'] == 'captured' else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('exe', 'isolation-root', 'output'):
        parser.add_argument('--' + option, required=True, type=Path)
    return run(parser.parse_args())


if __name__ == '__main__':
    sys.exit(main())
