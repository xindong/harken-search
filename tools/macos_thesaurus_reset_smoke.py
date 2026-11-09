#!/usr/bin/env python3
"""Exercise the same Mac thesaurus reset action used by Settings, in isolated data."""
import argparse
import json
from pathlib import Path

from macos_library_smoke import App, save


def run(binary: Path, root: Path, output: Path) -> bool:
    output.mkdir(parents=True, exist_ok=False)
    thesaurus = root / 'profile' / 'Application Support' / 'HarkenSearch' / 'harken_thesaurus.json'
    thesaurus.parent.mkdir(parents=True, exist_ok=True)
    custom = [{'triggers': ['qa_unique_trigger'], 'related': ['qa_unique_result']}]
    thesaurus.write_text(json.dumps(custom, ensure_ascii=False), encoding='utf-8')
    report = {'status': 'FAIL', 'scope': 'Mac Settings reset changes the active user thesaurus',
              'thesaurus': str(thesaurus), 'seed': custom}
    app = None
    try:
        app = App(binary, root, output)
        app.wait(lambda value: not value['switch_pending'], 'startup', 60)
        startup = dict(line.split('=', 1) for line in (app.runtime / 'startup.txt').read_text().splitlines() if '=' in line)
        assert Path(startup['settings']).parent == thesaurus.parent, startup
        assert json.loads(thesaurus.read_text(encoding='utf-8')) == custom
        app.command('show_thesaurus')
        app.command(capture=True)
        report['settings_before'] = str(output / f'{app.sequence:04d}.png')
        app.command('reset_thesaurus')
        defaults = json.loads(thesaurus.read_text(encoding='utf-8'))
        assert len(defaults) > 1 and custom[0] not in defaults, defaults
        assert all(group['triggers'] and group['related'] for group in defaults)
        report['default_rule_count'] = len(defaults)
        app.command(capture=True)
        report['settings_after'] = str(output / f'{app.sequence:04d}.png')
        report['first_exit'] = app.close()
        app.retain()
        app = None
        reopen = output / 'reopen'
        reopen.mkdir()
        app = App(binary, root, reopen)
        app.wait(lambda value: not value['switch_pending'], 'reopen', 60)
        assert json.loads(thesaurus.read_text(encoding='utf-8')) == defaults
        app.command(capture=True)
        report['reopen_exit'] = app.close()
        report['status'] = 'PASS'
    except Exception as error:
        report['error'] = str(error)
        if app is not None:
            try:
                report['exit_after_error'] = app.close()
            except Exception as close_error:
                report['close_error'] = str(close_error)
    finally:
        if app is not None:
            app.retain()
        save(output / 'report.json', report)
        print(json.dumps(report, ensure_ascii=False))
    return report['status'] == 'PASS'


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    raise SystemExit(0 if run(args.app.resolve(), args.root.resolve(), args.out.resolve()) else 1)
