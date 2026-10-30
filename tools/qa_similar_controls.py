"""Check Similar Sounds controls using an isolated EXE on a private desktop.
Input uses the package's own QA bridge, never the user's input desktop.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import time
from PIL import Image, ImageDraw, ImageFont
from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_shared_buttons import audit, contrast
from qa_transport_themes import Driver, THEMES
from qa_info_family_themes import resize_similar


def run(args):
    import win32con, win32event, win32gui, win32process
    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (root / 'profile/runtime').mkdir(parents=True, exist_ok=True)
    driver = Driver(root / 'profile/runtime', out)
    desktop = PrivateDesktop()
    process = None
    report = {'exe': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
              'started_at': datetime.now(timezone.utc).isoformat(), 'cases': [], 'failures': []}
    try:
        process, pid = desktop.start(exe)
        report['pid'] = pid
        ident = driver.command({'cmd': 'magic_eq_identity'}, timeout=60)
        assert ident['pid'] == pid and Path(ident['exe_path']).resolve() == exe, ident
        assert Path(ident['isolation_root']).resolve() == root, ident
        report['identity'] = ident
        hwnd = ident['hwnd']
        wait_state(driver, lambda s: s['main_ui_ready'] and not s['db_switch_in_progress'], 'Main UI ready', 60)
        win32gui.SetWindowPos(hwnd, 0, 40, 40, 1400, 900, win32con.SWP_NOACTIVATE | win32con.SWP_NOZORDER)

        def click(bounds):
            assert win32process.GetWindowThreadProcessId(hwnd)[1] == pid
            x, y = round((bounds[0] + bounds[2]) / 2), round((bounds[1] + bounds[3]) / 2)
            for phase in ('hover', 'down', 'up'):
                driver.command({'cmd': 'similar_index', 'action': 'pointer', 'phase': phase, 'x': x, 'y': y})
                time.sleep(.1)

        def choose(theme, label):
            popup = [x for x in audit(driver, theme)['checks'] if x['window'].startswith('##Combo_')]
            assert len(popup) == 5, popup
            for option in popup:
                assert contrast(option['fg'], option['fill']) >= 4.5, option
            click(next(x for x in popup if x['label'] == label)['bounds'])

        def observe(theme):
            current = audit(driver, theme)
            controls = [x for x in current['checks'] if 'SimilarActionRow' in x['window']]
            combo = next(x for x in controls if abs((x['bounds'][2] - x['bounds'][0]) - 132) < 1)
            find = next(x for x in controls if x is not combo)
            for c in controls:
                b, clip = c['bounds'], c['clip']
                assert b[0] >= clip[0] - 1 and b[1] >= clip[1] - 1 and b[2] <= clip[2] + 1 and b[3] <= clip[3] + 1, c
                if not c['disabled']:
                    c['contrast'] = contrast(c['fg'], c['fill'])
                    assert c['contrast'] >= 4.5, c
            assert abs(combo['bounds'][1] - find['bounds'][1]) <= 1, controls
            assert abs(combo['bounds'][3] - combo['bounds'][1] - 28) <= 1, combo
            assert abs(find['bounds'][3] - find['bounds'][1] - 28) <= 1, find
            return combo, find, current

        def shot(theme, state, observed):
            stem = f'theme-{theme}-{state}'
            full = out / (stem + '-full.png')
            capture(driver, full)
            panel = driver.command({'cmd': 'dock_state'})['panels']['similar-sounds']
            expected_width = 640 if state == 'enabled-wide' else 420
            assert abs(panel['w'] - expected_width) <= 2, (state, panel)
            origin = win32gui.ClientToScreen(hwnd, (0, 0))
            x, y = round(panel['x'] - origin[0]), round(panel['y'] - origin[1])
            w, h = round(panel['w']), min(round(panel['h']), 275)
            with Image.open(full) as frame:
                assert 0 <= x and 0 <= y and x+w <= frame.width and y+h <= frame.height, (panel, frame.size)
                frame.crop((x, y, x+w, y+h)).save(out / (stem + '.png'))
            report['cases'].append({'theme': theme, 'state': state, 'panel': panel, 'audit': observed, 'image': stem + '.png'})

        for theme, name in THEMES:
            driver.command({'cmd': 'set_theme', 'theme': theme})
            driver.command({'cmd': 'similar_index', 'action': 'open', 'floating': True, 'width': 420, 'height': 620, 'language': 0})
            time.sleep(.4)
            resize_similar(driver, 420)
            combo, find, observed = observe(theme)
            assert find['disabled'], 'Initial state must have no reference'
            shot(theme, 'disabled', observed)
            before = driver.command({'cmd':'similar_index','action':'state'})
            click(combo['bounds'])
            assert driver.command({'cmd':'similar_index','action':'state'})['open_popups'] > 0, 'Combo did not open'
            shot(theme, 'expanded', audit(driver, theme))
            choose(theme, '音色')
            changed, _, observed = observe(theme)
            assert changed['label'] == '音色', 'Click did not select the requested preset'
            selected = driver.command({'cmd':'similar_index','action':'state'})
            assert selected['preset'] == 1 and selected['open_popups'] == before['open_popups'], selected
            shot(theme, 'selected', observed)
            click(changed['bounds'])
            click(find['bounds'])
            escaped, _, _ = observe(theme)
            assert escaped['label'] == changed['label'], 'Clicking outside changed the selected preset'
            closed = driver.command({'cmd':'similar_index','action':'state'})
            assert closed['preset'] == selected['preset'] and closed['open_popups'] == before['open_popups'] and closed['rows'] == 0, closed
            click(find['bounds'])
            disabled_click = driver.command({'cmd':'similar_index','action':'state'})
            assert disabled_click['rows'] == 0 and disabled_click['open_popups'] == before['open_popups'], disabled_click
            click(escaped['bounds'])
            choose(theme, '均衡')
            print(f'{name}: alignment, contrast, selection, dismiss and disabled Find passed', flush=True)

        driver.command({'cmd': 'search', 'query': ''})
        wait_state(driver, lambda s: s.get('result_count', 0) > 0, 'Synthetic library results', 30)
        driver.command({'cmd': 'select_row', 'row': 0})
        time.sleep(.4)
        for theme, name in THEMES:
            driver.command({'cmd': 'set_theme', 'theme': theme})
            driver.command({'cmd': 'similar_index', 'action': 'open', 'floating': True, 'width': 640, 'height': 620, 'language': 0})
            time.sleep(.35)
            resize_similar(driver, 640)
            combo, find, observed = observe(theme)
            assert not find['disabled'], 'Reference selection must enable Find'
            shot(theme, 'enabled-wide', observed)
        click(find['bounds'])
        deadline = time.monotonic() + 30
        while True:
            state = driver.command({'cmd': 'similar_index', 'action': 'state'})
            if state['rows'] == 50: break
            assert time.monotonic() < deadline, state
            time.sleep(.1)
        report['real_search'] = state
        report['passed'] = True
    except Exception as exc:
        report['failures'].append(repr(exc))
        raise
    finally:
        try:
            if process and win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
                driver.command({'cmd': 'info_edit_input', 'action': 'exit'})
                assert win32event.WaitForSingleObject(process, 15000) == win32con.WAIT_OBJECT_0, 'QA instance did not exit'
                report['exit_code'] = win32process.GetExitCodeProcess(process)
                assert report['exit_code'] == 0, report['exit_code']
        finally:
            if process:
                report['exit_code'] = win32process.GetExitCodeProcess(process)
                process.Close()
            desktop.close()
            (out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    gallery(out, report)


def gallery(out, report):
    font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 18)
    canvas = Image.new('RGB', (890, len(THEMES)*319 + 50), '#edf2f6')
    draw = ImageDraw.Draw(canvas)
    draw.text((20, 12), '相似音效 · 统一按钮与下拉框', font=font, fill='#23374c')
    for i, (theme, name) in enumerate(THEMES):
        y = 50 + i*319
        draw.text((20, y), name + ' · 普通状态', font=font, fill='#23374c')
        draw.text((460, y), '下拉展开', font=font, fill='#23374c')
        for state, x in [('disabled', 20), ('expanded', 460)]:
            with Image.open(out / f'theme-{theme}-{state}.png') as im: canvas.paste(im, (x, y+30))
    canvas.save(out/'五主题按钮与下拉实拍.png')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    run(parser.parse_args())
