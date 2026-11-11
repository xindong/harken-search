#!/usr/bin/env python3
"""Exercise the real incremental build, restoring the owned notice in all cases."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser()
for name in ('repo', 'build', 'cmake', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
source = args.repo / 'THIRD_PARTY_NOTICES.md'
packaged = args.build / 'HarkenSearch.app/Contents/MacOS/THIRD_PARTY_NOTICES.md'
original = source.read_bytes()
marker = ('\n<!-- Harken isolated build dependency probe: ' + args.output.name + ' -->\n').encode()
report = {'status': 'FAIL', 'scope': 'actual incremental notice staging, no release',
          'source': str(source), 'original_sha256': hashlib.sha256(original).hexdigest()}

def build(label):
    command = [str(args.cmake), '--build', str(args.build), '--target', 'HarkenSearch', '--parallel', '2']
    with (args.output / (label + '.log')).open('wb') as log:
        result = subprocess.run(command, cwd=args.repo, stdout=log, stderr=subprocess.STDOUT)
    report[label] = {'command': command, 'exit_code': result.returncode}
    return result.returncode

try:
    subprocess.run(['git', 'diff', '--quiet', '--', source.name], cwd=args.repo, check=True)
    assert packaged.read_bytes() == original, 'baseline package must match its source notice'
    source.write_bytes(original + marker)
    assert build('changed_build') == 0, 'changed-notice build failed'
    report['changed_notice_staged'] = packaged.read_bytes() == original + marker
    assert report['changed_notice_staged'], 'successful incremental build retained stale notice'
    report['status'] = 'PASS'
except Exception as error:
    report['error'] = repr(error)
finally:
    source.write_bytes(original)
    # Never retain a marked application as a final candidate, even on failure.
    cleanup_exit = build('restored_build')
    report['source_restored'] = source.read_bytes() == original
    report['package_restored'] = packaged.read_bytes() == original
    report['restored_notice_sha256'] = hashlib.sha256(packaged.read_bytes()).hexdigest()
    if cleanup_exit != 0 or not report['source_restored'] or not report['package_restored']:
        report['status'] = 'FAIL'
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(report['status'], report.get('error', ''))
raise SystemExit(0 if report['status'] == 'PASS' else 1)
