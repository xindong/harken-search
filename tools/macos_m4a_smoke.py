#!/usr/bin/env python3
"""Use the real isolated Mac product and synthetic M4A fixtures."""
import argparse
import json
from pathlib import Path
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument('--app', type=Path, required=True)
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--fixtures', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--source-tools', type=Path,
                    default=Path(__file__).resolve().parent)
args = parser.parse_args()
for path in (args.app, args.root, args.fixtures, args.out):
    if not path.is_absolute() or path.resolve() != path:
        parser.error('app and test paths must be absolute and free of symlinks')
if 'qa' not in args.root.parts or 'qa' not in args.out.parts or not args.fixtures.is_relative_to(args.root):
    parser.error('test data, fixtures and output must use the isolated QA root')
sys.path.insert(0, str(args.source_tools))
from macos_library_smoke import App, save

out = args.out.resolve(strict=False)
out.mkdir(parents=True, exist_ok=False)
report = {'status': 'FAIL', 'scope': 'real Mac scan, M4A waveforms and audition', 'matrix': {}}
app = App(args.app, args.root, out)
try:
    app.wait(lambda s: not s['switch_pending'], 'startup')
    name = 'M4A-' + out.name
    app.command('create', name=name)
    app.wait(lambda s: name in s['database'] and not s['switch_pending'], 'create library')
    app.command('scan', path=str(args.fixtures))
    state = app.wait(lambda s: not s['scan']['active'] and not s['scan']['pending'] and not s['search_pending'] and s['count'] == 6, 'scan', 120)
    report['scan'] = state['scan']
    report['library'] = state['database']
    report['rows_after_scan'] = state['rows']
    app.command(capture=True)
    expected = {'wav': '基线 tone.wav', 'aac_m4a': '基线 AAC.m4a',
                'alac_m4a': '基线 ALAC.m4a', 'aiff': '基线 AIFF.aiff',
                'flac': '基线 FLAC.flac', 'damaged': 'damaged.m4a'}
    for label, name in expected.items():
        state = app.command()
        index, row = next((i, row) for i, row in enumerate(state['rows']) if row['name'] == name)
        app.command('select', index=index, play_when_ready=False)
        state = app.wait(lambda s: not s['audio']['pending'] and
                         (bool(s['audio']['error']) or
                          (s['audio']['selected_id'] == row['id'] and s['audio']['path'] == row['path'])),
                         label + ' audio load', 45)
        if not state['audio']['error']:
            try:
                state = app.wait(lambda s: s['audio']['wave_ready'] and s['audio']['wave_peaks'] > 0,
                                 label + ' waveform', 30)
            except AssertionError:
                state = app.command()
        report['matrix'][label] = {'row': row, 'audio': state['audio']}
        app.command(capture=True)
    app.command('filter', text='AAC')
    state = app.wait(lambda s: not s['search_pending'] and s['count'] == 1, 'M4A search')
    report['search'] = {'count': state['count'], 'filters': state['filters']}
    app.command('filter', text='')
    app.wait(lambda s: not s['search_pending'] and s['count'] == 6 and len(s['rows']) == 6,
             'clear search and restore rows')
    for label in ['aac_m4a', 'alac_m4a']:
        state = app.wait(lambda s: len(s['rows']) == 6, 'rows ready for playback')
        row = report['matrix'][label]['row']
        index = next(i for i, found in enumerate(state['rows']) if found['id'] == row['id'])
        app.command('volume', value=0.08)
        app.command('select', index=index, play_when_ready=False)
        ready = app.wait(lambda s: not s['audio']['pending'] and
                         (bool(s['audio']['error']) or
                          (s['audio']['selected_id'] == row['id'] and s['audio']['path'] == row['path'])),
                         label + ' reload')
        if ready['audio']['error']:
            report['matrix'][label]['transport_error'] = ready['audio']['error']
            continue
        before = ready['audio']['output']
        app.command('toggle_playback')
        playing = app.wait(lambda s: s['audio']['playing'] and s['audio']['position'] > 0.05,
                           label + ' play', 20)
        app.command('toggle_playback')
        paused = app.wait(lambda s: s['audio']['paused'] and not s['audio']['playing'], label + ' pause')
        app.command('seek', position=0.6)
        seeked = app.wait(lambda s: abs(s['audio']['position'] - 0.6) < 0.04, label + ' seek')
        app.command('toggle_playback')
        resumed = app.wait(lambda s: s['audio']['playing'] and s['audio']['position'] > 0.65,
                           label + ' resume', 20)
        app.command('stop')
        app.wait(lambda s: not s['audio']['playing'], label + ' stop')
        report['matrix'][label]['transport'] = {
            'before': before, 'playing': playing['audio'], 'paused': paused['audio'],
            'seeked': seeked['audio'], 'resumed': resumed['audio']}
        app.command(capture=True)
    state = app.wait(lambda s: len(s['rows']) == 6 and
                     all(row['mini_peaks'] > 0 for row in s['rows']
                         if row['name'] != 'damaged.m4a'),
                     'all valid list waveforms', 30)
    report['mini_waveforms'] = {row['name']: row['mini_peaks'] for row in state['rows']}
    report['exit_code'] = app.close()
    app.retain()
    reopen = out / 'reopen'
    reopen.mkdir()
    app = App(args.app, args.root, reopen)
    state = app.wait(lambda s: s['database'] == report['library'] and not s['switch_pending']
                     and not s['search_pending'] and s['count'] == 6, 'reopen library')
    report['reopen'] = {'database': state['database'], 'count': state['count']}
    app.command(capture=True)
    report['final_exit_code'] = app.close()
    problems = []
    for label in ('wav', 'aac_m4a', 'alac_m4a'):
        entry = report['matrix'][label]
        if not (entry['row']['duration'] > 2.8 and entry['row']['sample_rate'] == 44100 and entry['row']['channels'] == 2):
            problems.append(label + ': scan metadata')
        if entry['audio']['error'] or not entry['audio']['wave_ready'] or entry['audio']['wave_max'] <= 0.01:
            problems.append(label + ': decode or waveform')
    for label in ('aac_m4a', 'alac_m4a'):
        transport = report['matrix'][label].get('transport')
        if not transport or transport['playing']['output']['nonzero_samples'] <= transport['before']['nonzero_samples']:
            problems.append(label + ': audible output callback')
    if report['matrix']['damaged']['audio']['playing'] or not report['matrix']['damaged']['audio']['error']:
        problems.append('damaged M4A failure handling')
    if report['scan']['failed'] != 1:
        problems.append('scan failure count')
    report['problems'] = problems
    report['status'] = 'PASS' if not problems else 'FAIL'
except Exception as error:
    report['error'] = str(error)
    try:
        app.command(capture=True)
    except Exception as capture_error:
        report['capture_error'] = str(capture_error)
    try:
        report['exit_code'] = app.close()
    except Exception as close_error:
        report['shutdown_error'] = str(close_error)
        if app.process.poll() is None:
            app.process.terminate()
            app.process.wait(timeout=10)
            report['forced_exit'] = True
finally:
    app.retain()
    save(out / 'report.json', report)
print(json.dumps(report, ensure_ascii=False))
raise SystemExit(0 if report['status'] == 'PASS' else 1)
