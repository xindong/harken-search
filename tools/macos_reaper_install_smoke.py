#!/usr/bin/env python3
"""Run real installer transactions only in the card's dedicated fixture tree."""
import argparse
import hashlib
from pathlib import Path
import select
import shutil
import subprocess
from macos_library_smoke import App, save

parser = argparse.ArgumentParser()
for name in ('binary', 'component', 'host-fixture', 'root', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
fixture = args.root / 'fixtures/reaper-install' / args.output.name
fixture.mkdir(parents=True, exist_ok=False)
source = fixture / 'source/reaper_harken.dylib'
source.parent.mkdir()
shutil.copy2(args.component, source)
source_bytes = source.read_bytes()
expected = hashlib.sha256(source_bytes).hexdigest()
old_bytes = source_bytes + b'Synthetic previous contents; not a historical release.\n'
report = {'status': 'FAIL', 'scope': 'isolated real install transactions; not a Cockos host', 'cases': {}}
app = App(args.binary, args.root, args.output)
child = None

def target(case):
    return fixture / case / 'UserPlugins'

def invoke(case, directory=None, **fields):
    result = app.command('install_reaper_fixture', source=str(fields.pop('source', source)),
                         target_directory=str(directory or target(case)), **fields)['reaper_fixture_install']
    report['cases'][case] = result
    return result

def existing(directory, contents):
    directory.mkdir(parents=True)
    path = directory / source.name
    path.write_bytes(contents)
    return path

def stop_child():
    global child
    if child is not None:
        child.communicate('quit\n', timeout=10)
        report['controlled_process_exit'] = child.returncode
        assert child.returncode == 0, 'controlled process did not exit normally'
        child = None

try:
    app.wait(lambda s: not s['switch_pending'], 'startup', 60)
    outside = args.root / ('outside-install-fixture-' + args.output.name)
    try:
        app.command('install_reaper_fixture', source=str(source), target_directory=str(outside))
        raise RuntimeError('QA installer escaped its dedicated fixture area')
    except AssertionError as error:
        assert 'path_outside_reaper_install_fixture' in str(error), error
        assert not outside.exists()
        report['cases']['outside-fixture-rejected'] = {'expected_error': str(error), 'no_writes': True}
    result = invoke('dry-run', dry_run=True)
    assert result['ok'] and result['changed'] and not target('dry-run').exists(), result
    result = invoke('fresh')
    installed = target('fresh') / source.name
    assert result['ok'] and result['changed'] and result['verified'], result
    assert installed.read_bytes() == source_bytes
    result = invoke('repeat', directory=target('fresh'))
    assert result['ok'] and not result['changed'] and result['verified'], result
    result = invoke('simulated-running', host_running=True)
    assert not result['ok'] and result['failure_code'] == 'reaper-close-required', result
    assert not target('simulated-running').exists()
    untrusted = fixture / 'untrusted/reaper_harken.dylib'
    untrusted.parent.mkdir()
    untrusted.write_bytes(old_bytes)
    result = invoke('untrusted-source', source=untrusted)
    assert not result['ok'] and result['failure_code'] == 'bundled-metadata-invalid', result
    assert not target('untrusted-source').exists()
    unknown = existing(target('unknown-target'), b'Unrecognized file; must not overwrite.\n')
    before = unknown.read_bytes()
    result = invoke('unknown-target')
    assert not result['ok'] and result['failure_code'] == 'target-source-untrusted', result
    assert unknown.read_bytes() == before
    for checkpoint in ('stage', 'backup', 'replace', 'verify'):
        case = 'interrupt-' + checkpoint
        previous = existing(target(case), old_bytes)
        result = invoke(case, fail_at=checkpoint)
        assert not result['ok'] and not result['changed'], result
        assert previous.read_bytes() == old_bytes, 'interrupted installation lost previous bytes'
        assert not list(target(case).glob('*.tmp')), 'transaction temporary file leaked'
        if checkpoint in ('replace', 'verify'):
            assert result['rollback'] and result['rollback_verified'], result
        if result['backup_path']:
            assert Path(result['backup_path']).read_bytes() == old_bytes
    result = invoke('rollback-fresh', fail_at='replace')
    assert not result['ok'] and result['rollback'] and result['rollback_verified'], result
    assert not (target('rollback-fresh') / source.name).exists()
    assert not list(target('rollback-fresh').glob('*.tmp'))
    host = fixture / 'controlled-host/REAPER QA.app'
    executable = host / 'Contents/MacOS/REAPER'
    executable.parent.mkdir(parents=True)
    shutil.copy2(args.host_fixture, executable)
    child = subprocess.Popen([str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True)
    assert select.select([child.stdout], [], [], 10)[0], 'controlled process not ready'
    assert child.stdout.readline().strip() == 'READY'
    report['controlled_process_pid'] = child.pid
    result = invoke('native-running', detect_host=True, host_root=str(host))
    assert result['detected_running'] and not result['ok'], result
    assert result['failure_code'] == 'reaper-close-required' and not target('native-running').exists(), result
    stop_child()
    result = invoke('native-stopped', detect_host=True, host_root=str(host))
    assert not result['detected_running'] and result['ok'] and result['verified'], result
    assert (target('native-stopped') / source.name).read_bytes() == source_bytes
    assert hashlib.sha256(source.read_bytes()).hexdigest() == expected
    report['exit_code'] = app.close()
    report['source_sha256'] = expected
    report['status'] = 'PASS'
except Exception as error:
    report['error'] = repr(error)
    try:
        stop_child()
    except Exception as cleanup_error:
        report['controlled_process_cleanup_error'] = repr(cleanup_error)
        if child is not None and child.poll() is None:
            child.terminate()
            child.wait(timeout=10)
            report['controlled_process_forced_exit'] = True
    try:
        report['exit_code'] = app.close()
    except Exception as close_error:
        report['close_error'] = repr(close_error)
finally:
    app.retain()
    save(args.output / 'report.json', report)
print(report['status'], 'cases=', len(report['cases']), report.get('error', ''))
raise SystemExit(0 if report['status'] == 'PASS' else 1)
