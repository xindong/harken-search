"""Check that the actual navy EQ band distinguishes pass from notch.

Uses the rendered transport audit snapshots. Exit 0 on success, 1 on a
visual-contrast failure, and 2 when capture evidence is incomplete.
"""

import argparse
import json
import math
from pathlib import Path

from qa_transport_themes import contrast


def check(audit_dir: Path, output: Path) -> bool:
    report = json.loads((audit_dir / 'report.json').read_text(encoding='utf-8'))
    checks = []
    bands = {}
    for mode in ('pass', 'notch'):
        snapshot = json.loads((audit_dir / f'2-{mode}.json').read_text(encoding='utf-8'))
        if snapshot['theme'] != 2 or snapshot['schema'] != 2:
            raise ValueError(f'Unexpected navy EQ snapshot: {mode}')
        if not any(row['theme'] == 2 and row['scenario'] == mode and
                   row['frame'] == snapshot['frame'] for row in report['results']):
            raise ValueError(f'Snapshot absent from capture report: {mode}')
        center = next(row for row in snapshot['checks'] if row['id'] == 'eq.center')
        rail = next(row for row in snapshot['checks'] if row['id'] == 'eq.frequency')
        bands[mode] = center['bg']
        ratio = contrast(center['bg'], rail['bg'])
        checks.append({'name': f'{mode} band vs rail', 'ratio': ratio,
                       'pass': ratio >= 3.0})
    distance = math.dist(bands['pass'][:3], bands['notch'][:3])
    checks.append({'name': 'pass vs notch band color', 'distance': distance,
                   'pass': distance >= 45.0})
    result = {'identity': report['identity'], 'checks': checks,
              'pass': all(row['pass'] for row in checks)}
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(checks, ensure_ascii=False))
    return result['pass']


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--audit-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        raise SystemExit(0 if check(args.audit_dir, args.output) else 1)
    except (KeyError, StopIteration, OSError, ValueError) as error:
        print(error)
        raise SystemExit(2)
