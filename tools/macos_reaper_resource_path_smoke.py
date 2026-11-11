#!/usr/bin/env python3
"""Read-only real-product probes of synthetic Mac portable host directories."""
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
portable = fixture / 'Portable space 中文'
bundle = portable / 'REAPER QA.app'
executable = bundle / 'Contents/MacOS/REAPER'
executable.parent.mkdir(parents=True)
executable.write_text('Synthetic non-runnable path fixture, not Cockos.\n')
marker = portable / 'reaper.ini'
marker.write_text('[reaper]\n')
report = {'status': 'FAIL', 'scope': 'portable resource path only; no host launch or install',
          'probes': []}
app = App(args.binary, args.root, args.output)
try:
    app.wait(lambda s: not s['switch_pending'], 'startup', 60)
    for path in [bundle, executable]:
        probe = app.command('probe_reaper_host', path=str(path))['reaper_host_probe']
        report['probes'].append({'input': str(path), 'response': probe})
        assert probe['installed_path'] == str(portable / 'UserPlugins/reaper_harken.dylib'), probe
        assert not probe['send_ready'], probe
    marker.unlink()
    (bundle / 'reaper.ini').write_text('[reaper]\n')
    probe = app.command('probe_reaper_host', path=str(bundle))['reaper_host_probe']
    report['probes'].append({'input': str(bundle), 'bundle_marker_only': True, 'response': probe})
    assert probe['installed_path'] != str(bundle / 'UserPlugins/reaper_harken.dylib'), probe
    assert not probe['send_ready'], probe
    assert not list(fixture.rglob('UserPlugins')), 'diagnosis must not create plugin directories'
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
