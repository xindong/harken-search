#!/usr/bin/env python3
"""Exercise the product binary inspector, not a REAPER host or installer."""
import argparse
import hashlib
from pathlib import Path
import shutil
import struct
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
    assert info['trusted_source'] and info['version'] == '3.0.0.0' and info['protocol'] == 3, info
    assert info['manifest'], info
    # Preserve valid metadata/entry while changing the digest. Identity strings
    # cannot authenticate this copy, even with the managed plugin filename.
    tampered = fixture / 'tampered' / component.name
    tampered.parent.mkdir()
    tampered.write_bytes(component.read_bytes() + b'changed-digest\n')
    changed = app.command('inspect_reaper_binary', path=str(tampered))['reaper_binary_probe']
    report['tampered'] = changed
    assert changed['architecture'] == 'x64' and changed['recognized_entry'], changed
    assert changed['version'] == info['version'] and changed['protocol'] == info['protocol'], changed
    assert changed['sha256'] != expected and not changed['trusted_source'], changed
    # Synthetic universal wrappers of the actual built component. These are not hosts
    # and intentionally do not match the product's pinned whole-file digest.
    report['universal'] = []
    image = component.read_bytes()
    cpu, subtype = struct.unpack_from('<II', image, 4)
    for wide in (False, True):
        for endian in ('>', '<'):
            record = endian + ('IIQQII' if wide else 'IIIII')
            second = (4096 + len(image) + 4095) & ~4095
            container = bytearray(second + 32)
            struct.pack_into(endian + 'II', container, 0, 0xcafebabf if wide else 0xcafebabe, 2)
            for index, fields in enumerate(((cpu, subtype, 4096, len(image), 12),
                                             (0x0100000c, 0, second, 32, 12))):
                struct.pack_into(record, container, 8 + index * struct.calcsize(record),
                                 *(fields + (0,) if wide else fields))
            container[4096:4096 + len(image)] = image
            wrapper = fixture / f'universal-{wide}-{endian == "<"}' / component.name
            wrapper.parent.mkdir()
            wrapper.write_bytes(container)
            result = app.command('inspect_reaper_binary', path=str(wrapper))['reaper_binary_probe']
            report['universal'].append(result)
            assert result['architecture'] == 'x64' and result['recognized_entry'], result
            assert result['version'] == info['version'] and result['protocol'] == info['protocol'], result
            assert not result['trusted_source'], result
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
