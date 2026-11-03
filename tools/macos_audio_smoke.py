#!/usr/bin/env python3
"""Exercise native audition and waveforms in the real isolated Mac app."""
import argparse
import json
from pathlib import Path
import time

from macos_library_smoke import App, fixtures, save


def run(binary, root, output):
    output.mkdir(parents=True, exist_ok=False)
    app = App(binary, root, output)
    report = {'status': 'FAIL', 'scope': 'real native audition and waveforms'}
    try:
        app.wait(lambda s: not s['switch_pending'], 'startup library')
        name = 'Audio-' + output.name
        app.command('create', name=name)
        app.wait(lambda s: name in s['database'] and not s['switch_pending'], 'create audio library')
        source = root / 'fixtures' / ('audio-' + output.name)
        fixtures(source)
        app.command('scan', path=str(source))
        state = app.wait(lambda s: not s['scan']['active'] and not s['scan']['pending']
                         and not s['search_pending'] and s['count'] == 7, 'audio scan', 90)
        original_db = state['database']
        target = next(i for i, row in enumerate(state['rows'])
                      if row['name'] == 'stone-impact.wav' and '/A/' in row['path'])
        target_path = state['rows'][target]['path']
        app.command('volume', value=0.08)
        app.command('select', index=target, play_when_ready=False)
        state = app.wait(lambda s: s['audio']['loaded'] and not s['audio']['pending']
                         and s['audio']['path'] == target_path, 'selected file loads')
        assert not state['audio']['playing'], state['audio']
        state = app.wait(lambda s: s['audio']['wave_ready'] and s['audio']['wave_peaks'] > 100
                         and s['audio']['wave_max'] > 0.05, 'main waveform', 30)
        report['main_waveform'] = state['audio']
        output_before = state['audio']['output']
        assert output_before['device_started'] and output_before['backend'] == 'Core Audio', output_before
        app.command(capture=True)
        app.command('toggle_playback')
        playing = app.wait(lambda s: s['audio']['playing'] and s['audio']['position'] > 0.05, 'play advances')
        output_playing = playing['audio']['output']
        assert output_playing['callbacks'] > output_before['callbacks'], output_playing
        assert output_playing['nonzero_samples'] > output_before['nonzero_samples'], output_playing
        assert 0.00001 < output_playing['max_amplitude'] < 0.1, output_playing
        app.command('toggle_playback')
        paused = app.wait(lambda s: s['audio']['paused'] and not s['audio']['playing'], 'pause')
        time.sleep(0.3)
        still = app.command()
        assert abs(still['audio']['position'] - paused['audio']['position']) < 0.015, (paused, still)
        time.sleep(0.2)
        silent = app.command()
        assert silent['audio']['output']['callbacks'] > still['audio']['output']['callbacks'], silent
        assert silent['audio']['output']['nonzero_samples'] == still['audio']['output']['nonzero_samples'], (still, silent)
        report['output'] = {'before': output_before, 'playing': output_playing,
                            'paused': still['audio']['output'], 'still_paused': silent['audio']['output']}
        app.command('seek', position=0.65)
        seeked = app.wait(lambda s: abs(s['audio']['position'] - 0.65) < 0.03, 'seek while paused')
        assert not seeked['audio']['playing'], seeked['audio']
        app.command('toggle_playback')
        resumed = app.wait(lambda s: s['audio']['playing'] and s['audio']['position'] > 0.72, 'resume after seek')
        app.command('stop')
        # Shared stop() fades before stopping and keeps the cursor during that
        # path. Match the existing Windows contract: playback must stop moving.
        stopped = app.wait(lambda s: not s['audio']['playing'], 'stop after fade')
        time.sleep(0.3)
        after_stop = app.command()
        assert abs(after_stop['audio']['position'] - stopped['audio']['position']) < 0.015, (stopped, after_stop)
        report['transport'] = {label: s['audio'] for label, s in
                               [('playing', playing), ('paused', paused), ('seeked', seeked),
                                ('resumed', resumed), ('stopped', stopped)]}
        state = app.wait(lambda s: all(row['mini_peaks'] > 0 for row in s['rows']
                                      if row['name'] != 'damaged.wav'), 'visible mini waveforms', 20)
        report['mini_waveforms'] = [{'path': row['path'], 'peaks': row['mini_peaks']}
                                    for row in state['rows'] if row['name'] != 'damaged.wav']
        app.command(capture=True)
        good = [i for i, row in enumerate(state['rows']) if row['name'] != 'damaged.wav']
        for index in good[:3]:
            app.command('select', index=index, play_when_ready=False)
        final_path = state['rows'][good[2]]['path']
        state = app.wait(lambda s: not s['audio']['pending'] and s['audio']['loaded']
                         and s['audio']['path'] == final_path and s['audio']['wave_ready'], 'latest selection wins')
        report['rapid_selection'] = state['audio']
        broken = next(i for i, row in enumerate(state['rows']) if row['name'] == 'damaged.wav')
        app.command('select', index=broken, play_when_ready=False)
        failed = app.wait(lambda s: not s['audio']['pending'] and bool(s['audio']['error']), 'damaged file failure')
        assert not failed['audio']['playing'], failed['audio']
        report['damaged_file'] = failed['audio']
        app.command('select', index=target, play_when_ready=False)
        app.wait(lambda s: s['audio']['loaded'] and not s['audio']['pending']
                 and s['audio']['path'] == target_path and not s['audio']['error'], 'recover after failed audio')
        app.command('create', name=name + '-empty')
        empty = app.wait(lambda s: name + '-empty' in s['database'] and not s['switch_pending']
                         and not s['search_pending'] and s['count'] == 0, 'switch to empty library')
        assert not empty['audio']['loaded'] and not empty['audio']['playing'], empty
        assert empty['audio']['wave_peaks'] == 0, empty
        app.command('open', path=original_db)
        restored = app.wait(lambda s: s['database'] == original_db and not s['switch_pending']
                            and not s['search_pending'] and s['count'] == 7, 'return to original library')
        assert not restored['audio']['playing'] and not restored['audio']['loaded'], restored
        report['library_switch'] = {'empty': empty['audio'], 'returned': restored['audio']}
        report['exit_code'] = app.close()
        app.retain()
        reopened = output / 'reopened'
        reopened.mkdir()
        app = App(binary, root, reopened)
        restored = app.wait(lambda s: s['database'] == original_db and not s['switch_pending']
                            and not s['search_pending'] and s['count'] == 7, 'reopen audio library')
        assert not restored['audio']['playing'], restored
        report['restart'] = {'library': restored['database'], 'count': restored['count'], 'audio': restored['audio']}
        app.command(capture=True)
        report['final_exit_code'] = app.close()
        report['status'] = 'PASS'
    except Exception as error:
        report['error'] = str(error)
        try:
            app.command(capture=True)
        except Exception as error:
            report['capture_error'] = str(error)
        try:
            report['exit_code'] = app.close()
        except Exception as error:
            report['shutdown_error'] = str(error)
            if app.process.poll() is None:
                app.process.terminate()
                app.process.wait(timeout=10)
                report['forced_exit'] = True
    finally:
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
    for path in [args.app, args.root, args.out]:
        if not path.is_absolute() or path.resolve() != path:
            parser.error('absolute non-symlink paths required')
    if 'qa' not in args.root.parts or 'qa' not in args.out.parts:
        parser.error('dedicated qa roots required')
    raise SystemExit(0 if run(args.app, args.root, args.out) else 1)
