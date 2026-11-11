#!/usr/bin/env python3
"""Observe actual async Doctor-to-UI capability classification, never sends files."""
import argparse
from pathlib import Path
from macos_library_smoke import App, save, fixtures

parser = argparse.ArgumentParser()
for name in ('binary', 'root', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
fixture = args.root / 'fixtures' / args.output.name
fixture.mkdir(parents=True, exist_ok=False)
bundle = fixture / 'REAPER QA.app'
executable = bundle / 'Contents/MacOS/REAPER'
executable.parent.mkdir(parents=True)
executable.write_text('Synthetic non-runnable fixture, not Cockos.\n')
(fixture / 'reaper.ini').write_text('[reaper]\n')
report = {'status': 'FAIL', 'scope': 'real UI classification, no Ready handshake or send proof'}
app = App(args.binary, args.root, args.output)
try:
    app.wait(lambda s: not s['switch_pending'], 'startup', 60)
    name = 'Mac-' + args.output.name
    app.command('create', name=name)
    app.wait(lambda s: s['database_open'] and not s['switch_pending'] and name in s['database'],
             'create isolated screenshot library', 60)
    media = fixture / 'audio'
    fixtures(media)
    app.command('scan', path=str(media))
    app.wait(lambda s: not s['scan']['active'] and not s['scan']['pending'] and s['count'] == 7,
             'visible synthetic library', 90)
    app.command('reaper_capability_fixture', path=str(bundle))
    value = app.wait(lambda s: s['daw_capabilities'][0]['capability'] == 5,
                     'real missing-component diagnosis should require an adapter', 15)
    report['missing_component'] = value['daw_capabilities']
    assert value['daw_capabilities'][1]['capability'] == 8, value['daw_capabilities']
    app.command('reaper_capability_fixture', path=str(fixture / 'Missing.app'))
    value = app.wait(lambda s: s['daw_capabilities'][0]['capability'] == 9,
                     'missing host remains unconfigured', 15)
    report['missing_host'] = value['daw_capabilities']
    app.command('reaper_capability_fixture', path=str(bundle))
    app.wait(lambda s: s['daw_capabilities'][0]['capability'] == 5,
             'restore missing-component diagnostic', 15)
    value = app.command()
    index = next(i for i, row in enumerate(value['rows']) if row['name'] == 'stone-soft.wav')
    app.command('select', index=index)
    app.wait(lambda s: s['audio']['loaded'] and s['audio']['wave_ready'],
             'actual library audio and waveform ready', 60)
    value = app.command(capture=True)
    report['library'] = value['database']
    report['rows'] = len(value['rows'])
    report['screenshot'] = str(sorted(args.output.glob('*.png'))[-1])
    assert not list(fixture.rglob('UserPlugins')), 'classification must not install a plugin'
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
