#!/usr/bin/env python3
"""Probe actual running-process auto detection using only our controlled helper."""
import argparse
import select
import shutil
import subprocess
from pathlib import Path
from macos_library_smoke import App, save

parser = argparse.ArgumentParser()
for name in ('binary', 'host-fixture', 'root', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
fixture = args.root / 'fixtures' / args.output.name
fixture.mkdir(parents=True, exist_ok=False)
report = {'status': 'FAIL', 'scope': 'our controlled process only; no real Cockos host',
          'probes': {}, 'controlled_exits': []}
app = App(args.binary, args.root, args.output)
child = None

def probe(label):
    result = app.command('auto_detect_reaper_host')['reaper_auto_detect_probe']
    report['probes'][label] = result
    assert not result['send_ready'], 'controlled helper cannot prove a ready send bridge'
    return result

def start(executable):
    global child
    executable.parent.mkdir(parents=True)
    shutil.copy2(args.host_fixture, executable)
    child = subprocess.Popen([str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True)
    assert select.select([child.stdout], [], [], 10)[0], 'controlled process not ready'
    assert child.stdout.readline().strip() == 'READY'

def stop():
    global child
    if child is None:
        return
    child.communicate('quit\n', timeout=10)
    report['controlled_exits'].append(child.returncode)
    assert child.returncode == 0, 'controlled process did not exit normally'
    child = None

try:
    app.wait(lambda s: not s['switch_pending'], 'startup', 60)
    assert probe('before')['root'] == '', 'competing host found; do not operate it'
    bundle = fixture / 'Portable space 中文' / 'REAPER QA.app'
    executable = bundle / 'Contents/MacOS/REAPER'
    start(executable)
    (bundle.parent / 'reaper.ini').write_text('[reaper]\n')
    result = probe('running_bundle')
    assert result['root'] == str(bundle), result
    assert result['configured_root'] == str(bundle), result
    assert result['host_path'] == str(executable) and result['host_exists'] and result['host_running'], result
    assert result['resource_path'] == str(bundle.parent), result
    assert result['resolution'] == 'running_portable_install', result
    stop()
    assert probe('stopped_bundle')['root'] == ''
    start(fixture / 'loose/REAPER')
    assert probe('unsupported_layout')['root'] == '', 'unknown host layout must not become a configured app'
    stop()
    assert probe('final')['root'] == ''
    assert not list(fixture.rglob('UserPlugins')), 'read-only auto detection installed plugins'
    report['exit_code'] = app.close()
    report['status'] = 'PASS'
except Exception as error:
    report['error'] = repr(error)
    try:
        stop()
    except Exception as cleanup_error:
        report['cleanup_error'] = repr(cleanup_error)
        if child is not None and child.poll() is None:
            child.terminate()
            child.wait(timeout=10)
            report['forced_controlled_exit'] = True
    try:
        report['exit_code'] = app.close()
    except Exception as close_error:
        report['close_error'] = repr(close_error)
finally:
    app.retain()
    save(args.output / 'report.json', report)
print(report['status'], report.get('error', ''))
raise SystemExit(0 if report['status'] == 'PASS' else 1)
