"""Native scan-dialog smoke QA, using a private desktop and render-frame capture.

Requires Windows, pywin32 and Pillow. The EXE must be compiled for the supplied
isolation root; the CLI cannot redirect an ordinary EXE. Use a fresh profile or
an existing owned ScanDialogQA library, and a NEW output under D:/Harken-QA.
Wait for any other QA process using that root to exit. No input reaches the
user's desktop. Populated QA libraries are retained; a new QA library is created.

This first version requires --smoke: five themes, advanced expand/collapse,
real full scan of 24 deterministic WAVs, SQLite readback, Done and reopen.
Pause/revoke/failure/empty/similarity are explicitly NOT covered yet.
Exit 0: smoke checks passed, NOT human visual acceptance; 1: failed; 2: preflight.
Retains the isolated database, fixtures, PNGs, JSON evidence and command log.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import sqlite3
import struct
import sys
import time
import traceback
import uuid
import wave

from qa_required_library_setup import PrivateDesktop, capture
from qa_transport_themes import Driver, THEMES


def require(condition, message):
    # Safety checks must still work when Python is invoked with -O.
    if not condition:
        raise RuntimeError(message)


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def path_key(value):
    return os.path.normcase(str(Path(value).resolve())).replace(chr(92), '/').casefold()


def inside(path, parent):
    path, parent = path.resolve(), parent.resolve()
    return path != parent and path.is_relative_to(parent)


def save_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')


def make_wavs(folder, count=12, seconds=1):
    """Only create new fixture directories; never overwrite or delete audio."""
    folder.mkdir(parents=True, exist_ok=False)
    tones = {}
    for index in range(count):
        tone = index % 12
        if tone not in tones:
            tones[tone] = b''.join(struct.pack('<h', round(7000 * math.sin(
                2 * math.pi * (220 + tone * 17) * i / 16000))) for i in range(16000))
        samples = tones[tone]
        with wave.open(str(folder / f'sample-{index:04d}.wav'), 'wb') as sound:
            sound.setparams((1, 2, 16000, 0, 'NONE', 'not compressed'))
            for _ in range(seconds):
                sound.writeframes(samples)


class Smoke:
    def __init__(self, driver, root, output, timeout):
        self.driver, self.root, self.output, self.timeout = driver, root, output, timeout
        self.last = {}

    def state(self):
        state = self.driver.command({'cmd': 'scan_dialog_ui', 'action': 'state'})
        self.last = state
        keys = {'surface', 'phase', 'frame', 'theme', 'is_scanning', 'completion_pending',
                'pending_folders', 'last_folders', 'succeeded', 'failed', 'skipped',
                'unprocessed', 'items', 'windows', 'imgui_errors', 'imgui_debug_log'}
        require(keys <= state.keys(), 'scan_dialog_ui state 缺少字段：' + str(keys - state.keys()))
        require(state['imgui_errors'] == 0,
                f'ImGui 错误数不是 0：{state["imgui_errors"]}；日志尾部：{state["imgui_debug_log"]}')
        return state

    def wait(self, predicate, label, timeout=None, getter=None):
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        last = {}
        while time.monotonic() < deadline:
            last = (getter or self.state)()
            if predicate(last):
                return last
            time.sleep(.08)
        raise TimeoutError(f'{label}；最后状态：{json.dumps(last, ensure_ascii=False)}')

    @staticmethod
    def has(state, key):
        return any(item['id'] == key for item in state['items'])

    def target(self, state, key, window=None):
        matches = []
        for item in state['items']:
            if item['id'] != key:
                continue
            bounds = item['bounds']
            require(len(bounds) == 4 and all(math.isfinite(v) for v in bounds)
                    and bounds[0] < bounds[2] and bounds[1] < bounds[3], '无效控件 bounds')
            owners = []
            for candidate in state['windows']:
                box = candidate['bounds']
                if (window is None or window in candidate['name']) and (
                    box[0] - 1 <= bounds[0] and box[1] - 1 <= bounds[1]
                    and bounds[2] <= box[2] + 1 and bounds[3] <= box[3] + 1):
                    owners.append(candidate)
            if owners:
                declared = [w for w in owners if w['name'] == item.get('window')]
                owner = min(declared or owners, key=lambda w:
                            (w['bounds'][2] - w['bounds'][0]) * (w['bounds'][3] - w['bounds'][1]))
                matches.append((item, owner))
        require(len(matches) <= 1, f'控件不唯一，拒绝猜测点击：{key} / {window}')
        return matches[0] if matches else None

    def click(self, key, window=None):
        before = self.wait(lambda s: self.target(s, key, window) is not None,
                           f'未找到当前可见控件 {key}', timeout=15)
        # Native ImGui pointer injection, never SendInput or the OS mouse.
        for phase in ('hover', 'down', 'up'):
            # A live scan may change phase and reflow between pointer events.
            # Track the same semantic control, never reuse its old rectangle.
            current = self.wait(lambda s: self.target(s, key, window) is not None,
                                '阶段变化后控件已不可用：' + key, timeout=15)
            item, _ = self.target(current, key, window)
            left, top, right, bottom = item['bounds']
            self.driver.command({'cmd': 'scan_dialog_ui', 'action': 'pointer',
                                 'phase': phase, 'x': (left + right) / 2, 'y': (top + bottom) / 2})
            time.sleep(.08)
        return self.wait(lambda s: s['frame'] > before['frame'] + 1,
                         '点击后未取得新绘制帧', timeout=15)

    def shot(self, name, key, window=None):
        import ctypes
        from ctypes import wintypes
        import win32gui, win32process
        from PIL import Image
        before = self.state()
        target = self.target(before, key, window)
        require(target is not None, '截图目标窗口不存在：' + key)
        require(target[1]['no_title_bar'], '扫描窗口仍显示顶部标题条')
        path = self.output / (name + '.png')
        capture(self.driver, path)
        after = self.state()
        require(after['theme'] == before['theme'] and self.target(after, key, window) is not None,
                '截图期间主题或目标窗口变化，不能记为预期状态截图')
        identity = self.driver.command({'cmd':'magic_eq_identity'})
        require(path_key(identity['isolation_root'])==path_key(self.root), '截图时隔离身份改变')
        require(win32process.GetWindowThreadProcessId(identity['hwnd'])[1]==identity['pid'], '截图窗口身份改变')
        user32 = ctypes.WinDLL('user32', use_last_error=True)
        user32.SetThreadDpiAwarenessContext.argtypes=[wintypes.HANDLE]
        user32.SetThreadDpiAwarenessContext.restype=wintypes.HANDLE
        previous = user32.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
        try: origin = win32gui.ClientToScreen(identity['hwnd'], (0,0))
        finally:
            if previous: user32.SetThreadDpiAwarenessContext(previous)
        bounds = target[1]['bounds']
        crop = [round(bounds[0]-origin[0]),round(bounds[1]-origin[1]),round(bounds[2]-origin[0]),round(bounds[3]-origin[1])]
        closeup = self.output/(name+'-window.png')
        with Image.open(path) as frame:
            require(0 <= crop[0] < crop[2] <= frame.width and 0 <= crop[1] < crop[3] <= frame.height, '弹窗不在本次真实帧内，拒绝伪造截图')
            frame.crop(crop).save(closeup)
        evidence = {'png': str(path), 'window_png':str(closeup), 'client_origin':origin,
                    'sha256': sha256(path), 'before': before, 'after': after}
        save_json(self.output / (name + '.json'), evidence)
        return str(path)

    def create_library(self):
        get = lambda: self.driver.command({'cmd': 'get_state'})
        state = self.wait(lambda s: s.get('main_ui_ready') and not s.get('db_switch_in_progress'),
                          '程序未就绪', timeout=60, getter=get)
        if state.get('db_path'):
            database = Path(state['db_path']).resolve()
            require(inside(database, self.root / 'profile'), '已有库不在固定隔离 profile 内')
            require(database.parent.name == 'ScanDialogQA' or database.parent.name.startswith('ScanDialogQA-'),
                    '只允许复用本次授权的 ScanDialogQA 空库，不接管其他库')
            require(not state.get('new_db_dialog_open'), '已有库仍被建库窗口阻挡')
            if not self.rows(database):
                return database
            self.driver.command({'cmd': 'info_edit_input', 'action': 'new_db_dialog'})
        self.wait(lambda s: s.get('new_db_dialog_open'), '首次建库窗口未打开', timeout=15, getter=get)
        name = 'ScanDialogQA-' + uuid.uuid4().hex[:8]
        self.driver.command({'cmd': 'info_edit_input', 'action': 'new_db_name', 'text': name})
        time.sleep(.15)
        self.driver.command({'cmd': 'info_edit_input', 'action': 'new_db_create'})
        state = self.wait(lambda s: bool(s.get('db_path')) and not s.get('db_switch_in_progress')
                          and not s.get('new_db_dialog_open'), '独立库创建未完成', getter=get)
        database = Path(state['db_path']).resolve()
        require(inside(database, self.root / 'profile'), '新库不在已核对的隔离 profile 内')
        require(not self.rows(database), '新建测试库不是空库')
        return database

    def rows(self, database):
        require(inside(database, self.root / 'profile'), '拒绝读取非本包隔离数据库')
        with sqlite3.connect(database.as_uri() + '?mode=ro', uri=True, timeout=2) as connection:
            return {path_key(row[0]) for row in connection.execute('SELECT file_path FROM audio_files')}

    def setup(self, folders):
        before = self.state()
        self.driver.command({'cmd': 'scan_dialog_ui', 'action': 'setup',
                             'folders': [str(p) for p in folders], 'fast': False, 'similar': False})
        state = self.wait(lambda s: s['surface'] == 'setup' and s['frame'] > before['frame'] + 1
                          and self.has(s, 'scan.dialog.start'), '扫描入口未打开', timeout=15)
        require(not state['is_scanning'] and not state['completion_pending'], 'setup 意外启动了扫描')
        require([path_key(p) for p in state['pending_folders']] == [path_key(p) for p in folders],
                '入口中的目录与夹具不一致')
        return state

    def cancel(self):
        self.click('scan.dialog.cancel')
        return self.wait(lambda s: not self.has(s, 'scan.dialog.start') and not s['pending_folders']
                          and not s['is_scanning'], '取消后扫描入口未关闭', timeout=15)

    def themes(self, folders):
        evidence = []
        for theme, label in THEMES:
            # This isolated-only seam also sets ThemeSchedule mode Off. Ordinary
            # set_theme alone is overridden by the scheduled palette (theme=7).
            self.driver.command({'cmd': 'scan_dialog_ui', 'action': 'theme', 'theme': theme})
            self.setup(folders)
            self.wait(lambda s: s['theme'] == theme and not self.has(s, 'scan.option.recursive'),
                      '主题未应用或高级选项初始未折叠', timeout=15)
            shots = [self.shot(f'theme-{theme}-setup', 'scan.dialog.advanced')]
            self.click('scan.dialog.advanced')
            self.wait(lambda s: self.has(s, 'scan.option.recursive'), '高级选项未展开', timeout=15)
            shots.append(self.shot(f'theme-{theme}-advanced', 'scan.option.recursive'))
            self.click('scan.dialog.advanced')
            self.wait(lambda s: not self.has(s, 'scan.option.recursive'), '高级选项未收起', timeout=15)
            shots.append(self.shot(f'theme-{theme}-collapsed', 'scan.dialog.advanced'))
            self.cancel()
            evidence.append({'theme': theme, 'label': label, 'screenshots': shots})
            print(f'已采集 {label} 的入口、高级展开和折叠', flush=True)
        return evidence

    def completed(self, folders, database):
        self.setup(folders)
        self.click('scan.dialog.start')
        state = self.wait(lambda s: s['surface'] == 'result' and not s['is_scanning']
                          and not s['completion_pending'] and self.has(s, 'scan.result.done'),
                          '真实扫描没有产生完成结果')
        require(state['phase'] == 'completed' and state['succeeded'] == 24
                and all(state[k] == 0 for k in ('failed', 'skipped', 'unprocessed')),
                '完整扫描结果计数不符：' + json.dumps(state, ensure_ascii=False))
        require([path_key(p) for p in state['last_folders']] == [path_key(p) for p in folders],
                '完整扫描丢失多目录范围')
        expected = {path_key(p) for folder in folders for p in folder.glob('*.wav')}
        require(self.rows(database) == expected, '实际库记录与 24 个 WAV 夹具不一致')
        status = self.driver.command({'cmd': 'scan_status'})
        require(status['scan_strategy'] == 'full' and status['can_enter_completed']
                and not status['scan_completion_pending'], '完整扫描的完成条件未满足')
        image = self.shot('scan-completed', 'scan.result.done', '##ScanResult')
        self.click('scan.result.done', '##ScanResult')
        self.wait(lambda s: s['surface'] == 'setup' and not self.has(s, 'scan.result.done')
                  and not self.has(s, 'scan.dialog.start'), 'Done 未清空并关闭结果', timeout=15)
        # Navigation only: do not reset task/result state with a setup fixture.
        self.driver.command({'cmd': 'open_scan_dialog'})
        self.wait(lambda s: s['surface'] == 'setup' and self.has(s, 'scan.dialog.start')
                  and not s['is_scanning'], '再次打开仍然显示旧结果', timeout=15)
        reopened = self.shot('scan-reopened-setup', 'scan.dialog.start')
        self.cancel()
        return {'state': state, 'scan_status': status, 'database_rows': len(expected),
                'screenshots': [image, reopened]}


def run(args):
    require(sys.platform == 'win32', '需要 Windows、pywin32 和 Pillow')
    for key in ('exe', 'isolation_root', 'output'):
        value = getattr(args, key)
        require(value.is_absolute(), f'{key} 必须使用绝对路径')
        setattr(args, key, value.resolve())
    require(args.exe.is_file(), '指定 EXE 不存在')
    qa_root = Path('D:/Harken-QA').resolve()
    require(inside(args.isolation_root, qa_root) and inside(args.output, qa_root),
            '隔离数据和输出必须在 D:/Harken-QA 下')
    require(not args.output.exists(), '输出目录已存在，请换新目录，不覆盖旧证据')
    require(not args.isolation_root.is_relative_to(args.output)
            and not args.output.is_relative_to(args.isolation_root / 'profile'), '输出目录与隔离 profile 重叠')
    profile = args.isolation_root / 'profile'
    # Existing ScanDialogQA is accepted only after identity and an empty SQL readback.
    runtime = profile / 'runtime'
    require(not (runtime / 'qa_cmd.json').exists(), '隔离目录已有待处理命令，拒绝接管')
    require(args.timeout >= 15, 'timeout 不能少于 15 秒')
    import win32con
    import win32event
    import win32process
    from PIL import Image  # noqa: F401 -- check dependency before launch
    args.output.mkdir(parents=True, exist_ok=False)
    runtime.mkdir(parents=True, exist_ok=True)
    lock = runtime / 'qa_scan_dialog_simple.lock'
    with lock.open('x', encoding='utf-8') as file:
        file.write(json.dumps({'helper_pid': os.getpid(), 'output': str(args.output)}))
    report = {'status': 'running', 'started_at': utc_now(), 'mode': 'smoke',
              'exe': str(args.exe), 'exe_sha256': sha256(args.exe),
              'isolation_root': str(args.isolation_root), 'checks': [],
              'script_sha256': sha256(Path(__file__)), 'human_visual_acceptance': '未验收',
              'not_covered': ['暂停、撤销、失败、空目录和相似分析尚未实现',
                  '系统选择器、右键和拖入入口（本版 setup 仅准备输入，开始为实际 UI 点击）',
                  '窄窗口/DPI、文字内部裁切、X悬停/按下外观及人工视觉审核',
                  '所有中间进度阶段、后台继续、音频解码/磁盘/回滚故障']}
    driver = Driver(runtime, args.output)
    desktop = process = smoke = None
    verified = False
    current = None
    try:
        desktop = PrivateDesktop()
        process, pid = desktop.start(args.exe)
        # This MUST be the first bridge command; no get_state or mutation before it.
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=60)
        report['identity'] = identity
        require(identity.get('pid') == pid, 'identity PID 与刚启动的私有进程不一致')
        require(path_key(identity.get('exe_path', '')) == path_key(args.exe), 'identity EXE 不一致')
        require(path_key(identity.get('isolation_root', '')) == path_key(args.isolation_root),
                'identity 固定隔离目录不一致；参数不能重定向普通包')
        require(path_key(identity.get('runtime_root', '')) == path_key(runtime), 'identity runtime root 不一致')
        verified = True
        smoke = Smoke(driver, args.isolation_root, args.output, args.timeout)

        def case(name, action):
            nonlocal current
            current = {'name': name, 'status': 'running', 'started_at': utc_now()}
            report['checks'].append(current)
            current['evidence'] = action()
            current.update(status='passed', finished_at=utc_now())
            save_json(args.output / 'report.json', report)
            return current['evidence']

        database = Path(case('create-isolated-library', lambda: str(smoke.create_library())))
        report['database'] = str(database)
        fixtures = args.isolation_root / ('scan-dialog-fixtures-' + uuid.uuid4().hex[:12])
        folders = [fixtures / 'complete-a', fixtures / 'complete-b']
        for folder in folders:
            make_wavs(folder)
        report['fixtures'] = {'folders': [str(p) for p in folders], 'files': 24,
                              'format': 'mono PCM16 / 16000 Hz / 1 second / deterministic sine'}
        case('five-themes-setup-advanced-collapse', lambda: smoke.themes(folders))
        case('full-scan-completed-and-reopen', lambda: smoke.completed(folders, database))
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error), traceback=traceback.format_exc())
        if current is not None and current['status'] == 'running':
            current.update(status='failed', error=str(error))
        if smoke is not None:
            report['last_observed_state'] = smoke.last
        if verified:
            try:
                capture(driver, args.output / 'failure-diagnostic.png')
            except Exception as capture_error:
                report['diagnostic_capture_error'] = str(capture_error)
    finally:
        try:
            if process is not None:
                if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
                    if verified:
                        try:
                            driver.command({'cmd': 'info_edit_input', 'action': 'exit'}, timeout=8)
                        except Exception as error:
                            report['exit_command_error'] = str(error)
                    if win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_TIMEOUT:
                        # Only our CreateProcess handle, never a name/PID lookup.
                        win32process.TerminateProcess(process, 1)
                        require(win32event.WaitForSingleObject(process, 5000) == win32con.WAIT_OBJECT_0,
                                '私有 QA 进程未退出，保留隔离现场')
                        report['forced_private_process_exit'] = True
                        report['status'] = 'failed'
                report['process_exit_code'] = win32process.GetExitCodeProcess(process)
                if report['process_exit_code'] != 0:
                    report['status'] = 'failed'
                process.Close()
                process = None
            require(sha256(args.exe) == report['exe_sha256'], '测试期间 EXE 被替换')
        except Exception as error:
            report.update(status='failed', cleanup_error=str(error))
        finally:
            if desktop is not None:
                desktop.close()
            if process is None:
                lock.unlink()  # Only this invocation's exact lock; retain all data/evidence.
            report['finished_at'] = utc_now()
            save_json(args.output / 'report.json', report)
    print(json.dumps({'status': report['status'], 'report': str(args.output / 'report.json'),
                      'human_visual_acceptance': '未验收'}, ensure_ascii=False), flush=True)
    return 0 if report['status'] == 'passed' else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('exe', 'isolation-root', 'output'):
        parser.add_argument('--' + option, type=Path, required=True)
    parser.add_argument('--smoke', action='store_true', help='本版仅实现 smoke')
    parser.add_argument('--timeout', type=float, default=180, help='业务等待超时秒数，默认180')
    args = parser.parse_args()
    if not args.smoke:
        parser.error('当前版本只实现 smoke，请传 --smoke；其他路径不会冒充已覆盖')
    try:
        return run(args)
    except Exception as error:
        print(f'扫描 QA 未能开始或收尾失败：{error}。请核对固定隔离包、依赖和新输出目录后重试。', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
