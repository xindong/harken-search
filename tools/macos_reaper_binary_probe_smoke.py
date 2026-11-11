#!/usr/bin/env python3
"""Exercise the product binary inspector, not a REAPER host or installer."""
import argparse
import hashlib
from pathlib import Path
import shutil
from macos_library_smoke import App, save

parser = argparse.ArgumentParser()
parser.add_argument('--binary', type=Path, required=True)
parser.add_argument('--component', type=Path, required=True)
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
fixture = args.root / 'fixtures' / args.output.name
fixture.mkdir(parents=True, exist_ok=False)
component = fixture / 'reaper_harken.dylib'
shutil.copy2(args.component, component)
expected = hashlib.sha256(component.read_bytes()).hexdigest()
damaged = fixture / 'damaged.dylib'
damaged.write_text('This is not a Mach-O image.\n')
app = App(args.binary, args.root, args.output)
report = {'status': 'FAIL', 'scope': 'real product static binary inspection only'}
try:
    app.wait(lambda s: not s['switch_pending'], 'startup', 60)
    info = app.command('inspect_reaper_binary', path=str(component))['reaper_binary_probe']
    report['component'] = info
    assert info['exists'] and info['architecture'] == 'x64', info
    assert info['sha256'] == expected and info['recognized_entry'], info
    assert not info['trusted_source'] and not info['version'] and not info['protocol'], info
    # Native architecture/entry are facts, not proof of source trust or metadata.
    bad = app.command('inspect_reaper_binary', path=str(damaged))['reaper_binary_probe']
    report['damaged'] = bad
    assert bad['exists'] and not bad['architecture'] and not bad['trusted_source'], bad
    assert not bad['recognized_entry'], bad
    assert hashlib.sha256(component.read_bytes()).hexdigest() == expected
    assert not list(fixture.rglob('UserPlugins'))
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
