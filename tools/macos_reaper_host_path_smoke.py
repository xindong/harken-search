#!/usr/bin/env python3
"""Probe real doctor paths using non-runnable fixtures; never starts a DAW."""
import argparse
from pathlib import Path
from macos_library_smoke import App, save

parser = argparse.ArgumentParser()
parser.add_argument('--binary', type=Path, required=True)
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
fixture = args.root / 'fixtures' / args.output.name
fixture.mkdir(parents=True, exist_ok=False)
bundle = fixture / 'REAPER 中文 QA.app'
executable = bundle / 'Contents/MacOS/REAPER'
executable.parent.mkdir(parents=True)
executable.write_text('Non-runnable synthetic fixture. Path resolution only.\n')
missing_exe = fixture / 'Missing executable.app'
missing_exe.mkdir()
report = {'status': 'FAIL', 'scope': 'real product doctor path resolution only', 'probes': []}
app = App(args.binary, args.root, args.output)
try:
    app.wait(lambda s: not s['switch_pending'], 'startup', 60)
    for path, expected_root, expected_exe, root_exists, host_exists in [
        (bundle, bundle, executable, True, True),
        (executable, bundle, executable, True, True),
        (missing_exe, missing_exe, missing_exe / 'Contents/MacOS/REAPER', True, False),
        (fixture / 'Absent.app', fixture / 'Absent.app',
         fixture / 'Absent.app/Contents/MacOS/REAPER', False, False),
    ]:
        probe = app.command('probe_reaper_host', path=str(path))['reaper_host_probe']
        report['probes'].append({'input': str(path), 'response': probe})
        assert probe['configured'], probe
        assert probe['root'] == str(expected_root), probe
        assert probe['executable'] == str(expected_exe), probe
        assert probe['root_exists'] == root_exists, probe
        assert probe['host_exists'] == host_exists, probe
        assert not probe['send_ready'], 'a fixture must never count as a ready send bridge'
    assert not list(fixture.rglob('UserPlugins')), 'read-only diagnosis installed a plugin'
    report['exit_code'] = app.close()
    report['status'] = 'PASS'
except Exception as error:
    report['error'] = repr(error)
    try:
        report['exit_code'] = app.close()
    except Exception as close_error:
        report['close_error'] = repr(close_error)
finally:
    app.retain()
    save(args.output / 'report.json', report)
print(report['status'], report.get('error', ''))
raise SystemExit(0 if report['status'] == 'PASS' else 1)
