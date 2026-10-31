"""Check navy EQ state separation using real transport audit snapshots.

Exit 0: active/bypass button surfaces differ >=3:1 and text remains >=4.5:1.
Exit 1: failed checks. Exit 2: incomplete or inconsistent capture.
"""
import argparse
import json
from pathlib import Path
from qa_transport_themes import contrast


def run(args):
    snapshots = {name: json.loads((args.audit_dir / ('2-' + name + '.json')).read_text(encoding='utf-8'))
                 for name in ['pass', 'notch', 'bypass']}
    report = json.loads((args.audit_dir / 'report.json').read_text(encoding='utf-8'))
    rows = []
    for name, snapshot in snapshots.items():
        if snapshot['theme'] != 2 or snapshot['schema'] != 2:
            raise ValueError('Unexpected theme/schema')
        if not any(r['theme'] == 2 and r['scenario'] == name and r['frame'] == snapshot['frame'] for r in report['results']):
            raise ValueError('Snapshot does not match capture report')
    for control in ['eq.label', 'eq.mode']:
        values = {}
        for name, snapshot in snapshots.items():
            values[name] = next(c for c in snapshot['checks'] if c['id'] == control)
            ratio = contrast(values[name]['fg'], values[name]['bg'])
            rows.append({'control': control, 'check': name + ' text', 'ratio': ratio, 'pass': ratio >= 4.5})
        for name in ['pass', 'notch']:
            ratio = contrast(values[name]['bg'], values['bypass']['bg'])
            rows.append({'control': control, 'check': name + ' vs bypass surface', 'ratio': ratio, 'pass': ratio >= 3})
    result = {'identity': report['identity'], 'checks': rows, 'pass': all(r['pass'] for r in rows)}
    args.output.write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result['checks']))
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--audit-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    try:
        raise SystemExit(run(parser.parse_args()))
    except Exception as error:
        print(str(error))
        raise SystemExit(2)
