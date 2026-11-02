#!/usr/bin/env python3
"""Exercise the real isolated Cocoa product without moving the user's pointer.

The executable must be built with ARIS_PREVIEW_ISOLATION, diagnostics and this
exact QA root. This checks entry/settings/exit only, not the audio-library flow.
All attempts and screenshots are retained; a timed-out process is a failure.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True, help='Contents/MacOS/HarkenSearch')
    parser.add_argument('--qa-root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    root = args.qa_root
    if not root.is_absolute() or root.resolve() != root or 'qa' not in root.parts:
        raise RuntimeError('Use an absolute, non-symlink QA directory')
    if not args.app.is_file() or args.out.exists():
        raise RuntimeError('Executable required; evidence output must be a new directory')
    args.out.mkdir(parents=True)
    runtime = root / 'runtime'
    profile = root / 'profile' / 'Application Support' / 'HarkenSearch'
    settings = profile / 'settings.ini'
    receipts = []

    def run(name, expected_exit=0, deny_settings_write=False):
        evidence = args.out / name
        evidence.mkdir()
        events_file = runtime / 'events.jsonl'
        start = events_file.stat().st_size if events_file.exists() else 0
        start_snapshot = (runtime / 'startup.txt').stat().st_size if (runtime / 'startup.txt').exists() else 0
        before_settings = settings.read_bytes() if settings.exists() else None
        original_mode = None
        changed_mode = False
        started = time.monotonic()
        command = [str(args.app), '--mac-qa-smoke']
        with (evidence / 'stdout.log').open('wb') as stdout, (evidence / 'stderr.log').open('wb') as stderr:
            process = subprocess.Popen(command, cwd=args.out, stdout=stdout, stderr=stderr)
            timed_out = False
            try:
                while process.poll() is None:
                    if deny_settings_write and not changed_mode and events_file.exists():
                        recent = events_file.read_bytes()[start:].decode('utf-8')
                        if '"event":"ProductInitialized"' in recent:
                            if profile.resolve() != profile or not profile.is_relative_to(root):
                                raise RuntimeError('Refuse permissions change outside isolated profile')
                            original_mode = profile.stat().st_mode & 0o777
                            profile.chmod(0o500)
                            changed_mode = True
                    if time.monotonic() - started > 30:
                        timed_out = True
                        process.terminate()
                        process.wait(timeout=5)
                        break
                    time.sleep(0.02)
            finally:
                if original_mode is not None:
                    profile.chmod(original_mode)
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
        text = events_file.read_bytes()[start:].decode('utf-8') if events_file.exists() else ''
        (evidence / 'events.jsonl').write_text(text, encoding='utf-8')
        events = [json.loads(line)['event'] for line in text.splitlines()]
        snapshot = (runtime / 'startup.txt').read_bytes()[start_snapshot:].decode('utf-8')
        (evidence / 'startup.txt').write_text(snapshot, encoding='utf-8')
        for source in (runtime / 'main-window.png', settings, profile / 'imgui.ini'):
            if source.exists():
                shutil.copy2(source, evidence / source.name)
        result = {'name': name, 'pid': process.pid, 'command': command,
                  'exit_code': process.returncode, 'expected_exit': expected_exit,
                  'duration_seconds': round(time.monotonic() - started, 3),
                  'timed_out': timed_out, 'settings_write_denied': changed_mode,
                  'events': events, 'binary_sha256': hashlib.sha256(args.app.read_bytes()).hexdigest()}
        (evidence / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        receipts.append(result)
        assert not timed_out and process.returncode == expected_exit, result
        assert f'settings={settings}' in snapshot, snapshot
        assert f'default_library_directory={profile / "Database"}' in snapshot, snapshot
        for stage in ('ProductInitialized', 'RealMainUiRendered', 'ScreenshotSaved', 'NormalCloseRequested',
                      'DatabaseSwitchThreadJoinEnd', 'WaveformWorkerStopEnd', 'SearchWorkerStopEnd',
                      'DatabaseCloseEnd', 'AudioEngineShutdownEnd', 'ShutdownComplete'):
            assert events.count(stage) == 1, (stage, events)
        assert events.index('DatabaseSwitchThreadJoinEnd') < events.index('DatabaseCloseEnd')
        assert events.index('WaveformWorkerStopEnd') < events.index('DatabaseCloseEnd')
        if deny_settings_write:
            assert changed_mode and 'SettingsSaveFailed' in events
            assert settings.read_bytes() == before_settings, 'Failed save modified old settings'
        else:
            assert 'SettingsSaveEnd' in events and 'SettingsSaveFailed' not in events
        return snapshot

    try:
        run('01-baseline')
        content = settings.read_text(encoding='utf-8')
        changed, count = re.subn(r'^readingFontSize=\d+$', 'readingFontSize=18', content, flags=re.M)
        assert count == 1, 'Expected one readingFontSize setting'
        settings.write_text(changed, encoding='utf-8')
        assert 'reading_font=18' in run('02-reopen-changed-setting')
        assert re.search(r'^readingFontSize=18$', settings.read_text(encoding='utf-8'), re.M)
        run('03-save-failure', expected_exit=3, deny_settings_write=True)
        assert 'reading_font=18' in run('04-recover-and-reopen')
        report = {'status': 'PASS', 'scope': 'real UI entry, isolated paths, settings restart, failed-save preservation, normal shutdown', 'runs': receipts}
    except Exception as error:
        report = {'status': 'FAIL', 'error': str(error), 'runs': receipts}
        raise
    finally:
        (args.out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(report, ensure_ascii=False))


if __name__ == '__main__':
    main()
