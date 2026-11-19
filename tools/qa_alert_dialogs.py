"""Capture every representative alert dialog in five themes on an isolated QA build.

The EXE must be compiled with ARIS_PREVIEW_ISOLATION=ON and pinned to the
isolation root given on the command line. All dialogs are opened through the
preview-only QA command queue; no user desktop or user library is touched.

Exit 0: every dialog passes the rounded/no-separator/readability checks.
Exit 1: at least one visual check failed.
Exit 2: fixture, app or capture error.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
import time
import uuid

from PIL import Image

from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_scan_speed import contains_root_literal
from qa_transport_themes import Driver

ROOT = Path(__file__).resolve().parents[1]
LEDGER = json.load(io.open(ROOT / 'locale' / 'harken.ui.strings.json', encoding='utf-8'))['entries']
UI_TEXT = {entry['id']: entry['zh'] for entry in LEDGER}

DIALOGS = [
    ('close-choice', 'Dlg_CloseSM', '\u5173\u95ed\u786e\u8ba4\uff1a\u591a\u9009\u9879\u4e0e\u8bb0\u4f4f\u52fe\u9009'),
    ('rename', 'Dlg_EditFilename', '\u91cd\u547d\u540d\uff1a\u8f93\u5165\u6846\u4e0e\u786e\u5b9a/\u53d6\u6d88'),
    ('remove-records', 'Dlg_ConfirmRemove', '\u79fb\u9664\u8bb0\u5f55\uff1a\u53ea\u79fb\u9664\u6570\u636e\u5e93\u8bb0\u5f55'),
    ('delete-files', 'Dlg_ConfirmDelete', '\u5220\u9664\u6587\u4ef6\uff1a\u7ea2\u8272\u98ce\u9669\u63d0\u793a\u4e0e\u6587\u4ef6\u6e05\u5355'),
    ('new-playlist', 'Dlg_NewPlaylist', '\u65b0\u5efa\u64ad\u653e\u5217\u8868\uff1a\u540d\u79f0\u8f93\u5165'),
    ('new-folder', 'Dlg_NewFolder', '\u65b0\u5efa\u6587\u4ef6\u5939\uff1a\u540d\u79f0\u8f93\u5165'),
]
THEMES = [(0, 'Dark', '\u7384\u591c'), (1, 'Ocean', '\u6781\u5149'),
          (2, 'Lapis', '\u7409\u7483\u84dd'), (3, 'Jade', '\u9701\u7fe0'),
          (10, 'Rose', '\u73ab\u7470\u96ea\u677e')]


def luminance(color):
    channels = []
    for value in color[:3]:
        value /= 255.0
        channels.append(value / 12.92 if value <= .04045 else ((value + .055) / 1.055) ** 2.4)
    return .2126 * channels[0] + .7152 * channels[1] + .0722 * channels[2]


def contrast(a, b):
    lo, hi = sorted((luminance(a), luminance(b)))
    return (hi + .05) / (lo + .05)


def distance(a, b):
    return max(abs(x - y) for x, y in zip(a[:3], b[:3]))


def analyze(image, bounds, title):
    rgb = image.convert('RGB')
    x0, y0, x1, y1 = [int(round(v)) for v in bounds]
    x0, y0 = max(2, x0), max(2, y0)
    x1, y1 = min(image.width - 3, x1), min(image.height - 3, y1)
    if x1 - x0 < 120 or y1 - y0 < 60:
        raise AssertionError(f'dialog bounds too small: {bounds}')
    fill = rgb.getpixel((x0 + 30, y0 + (y1 - y0) // 2))
    border_counts = {}
    for y in range(y0 + 10, y1 - 10):
        pixel = rgb.getpixel((x0, y))
        if distance(pixel, fill) >= 8:
            border_counts[pixel] = border_counts.get(pixel, 0) + 1
    border = max(border_counts, key=border_counts.get) if border_counts else None
    rect = [x0, y0, x1, y1]
    refined_ok = False
    if border is not None:
        def row_fraction(y):
            hits = total = 0
            for x in range(x0 + 20, x1 - 20, 2):
                total += 1
                hits += 1 if distance(rgb.getpixel((x, y)), border) <= 6 else 0
            return hits / max(1, total)

        def col_fraction(x):
            hits = total = 0
            for y in range(y0 + 20, y1 - 20, 2):
                total += 1
                hits += 1 if distance(rgb.getpixel((x, y)), border) <= 6 else 0
            return hits / max(1, total)

        tops = [y for y in range(max(2, y0 - 90), min(image.height - 2, y0 + 24))
                if row_fraction(y) >= .8]
        bottoms = [y for y in range(max(2, y1 - 24), min(image.height - 2, y1 + 90))
                   if row_fraction(y) >= .8]
        lefts = [x for x in range(max(2, x0 - 90), min(image.width - 2, x0 + 24))
                 if col_fraction(x) >= .8]
        rights = [x for x in range(max(2, x1 - 24), min(image.width - 2, x1 + 90))
                  if col_fraction(x) >= .8]
        if tops and bottoms and lefts and rights:
            refined = [min(lefts), min(tops), max(rights), max(bottoms)]
            if abs((refined[3] - refined[1]) - (y1 - y0)) <= 80 and                abs((refined[2] - refined[0]) - (x1 - x0)) <= 80:
                rect = refined
                refined_ok = True
    x0, y0, x1, y1 = rect
    width, height = x1 - x0, y1 - y0
    if width < 120 or height < 60:
        raise AssertionError(f'dialog bounds too small after refinement: {rect}')
    counts = {}
    for y in range(y0 + 3, y1 - 2):
        for x in range(x0 + 3, x1 - 2):
            pixel = rgb.getpixel((x, y))
            counts[pixel] = counts.get(pixel, 0) + 1
    background = max(counts, key=counts.get)
    scrim = rgb.getpixel((max(0, x0 - 6), max(0, y0 - 6)))
    corner = rgb.getpixel((x0 + 2, y0 + 2))
    panel = rgb.getpixel((x0 + 30, y0 + height // 2))
    top_border = border is not None and distance(
        rgb.getpixel((x0 + width // 2, y0)), border) <= 12
    near_border = 0
    if border is not None:
        for y in range(y0, y0 + 6):
            for x in range(x0, x0 + 6):
                if distance(rgb.getpixel((x, y)), border) <= 10:
                    near_border += 1
    rounded = bool(refined_ok and top_border and near_border <= 3)

    separators = []
    for y in range(y0 + 26, min(y1 - 24, y0 + 96)):
        row = {}
        for x in range(x0 + 6, x1 - 6):
            pixel = rgb.getpixel((x, y))
            row[pixel] = row.get(pixel, 0) + 1
        common, count = max(row.items(), key=lambda item: item[1])
        fraction = count / max(1, sum(row.values()))
        if fraction < .85 or distance(common, background) < 10:
            continue
        above = rgb.getpixel((x0 + width // 2, max(y0, y - 4)))
        below = rgb.getpixel((x0 + width // 2, min(y1, y + 4)))
        if distance(common, above) >= 10 and distance(common, below) >= 10:
            separators.append(y)

    text_pixels = 0
    ratios = []
    title_pixels = 0
    for y in range(y0 + 3, y1 - 2):
        for x in range(x0 + 3, x1 - 2):
            pixel = rgb.getpixel((x, y))
            if pixel == background:
                continue
            ratio = contrast(pixel, background)
            if ratio >= 2.5:
                text_pixels += 1
                ratios.append(ratio)
                if y <= y0 + 44:
                    title_pixels += 1
    ratios.sort()
    p95 = ratios[int(len(ratios) * .95) - 1] if ratios else 0.0
    max_ratio = ratios[-1] if ratios else 0.0
    return {
        'title': title,
        'reported_bounds': [int(round(v)) for v in bounds],
        'bounds': [x0, y0, x1, y1],
        'background': list(background),
        'scrim': list(scrim),
        'corner': list(corner),
        'panel': list(panel),
        'rounded_corner': bool(rounded),
        'border': list(border) if border is not None else None,
        'refined_ok': bool(refined_ok),
        'top_border': bool(top_border),
        'corner_border_pixels': near_border,
        'separator_rows': separators,
        'text_pixels': text_pixels,
        'title_pixels': title_pixels,
        'contrast_p95': round(p95, 2),
        'contrast_max': round(max_ratio, 2),
    }

def run(args):
    import win32con
    import win32event
    import win32process

    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    assert exe.is_file(), f'missing EXE: {exe}'
    assert contains_root_literal(exe, root), 'QA EXE is not pinned to this isolation root'
    out.mkdir(parents=True, exist_ok=False)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    driver = Driver(runtime, out)
    desktop = PrivateDesktop()
    report = {
        'exe': str(exe),
        'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
        'isolation_root': str(root),
        'started_at': datetime.now(timezone.utc).isoformat(),
        'themes': [{'id': theme, 'key': key, 'name': name} for theme, key, name in THEMES],
        'dialogs': [],
        'passed': False,
    }
    process = None
    identity = None
    verified = False

    def save():
        (out / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

    def command(**request):
        return driver.command(request, timeout=60)

    def popup_for(title, timeout=12):
        deadline = time.monotonic() + timeout
        seen = None
        while time.monotonic() < deadline:
            ui = command(cmd='menu_ui')
            seen = list(ui.get('popups', []))
            for item in seen:
                if item['name'].split('##')[0] == title:
                    return item
            time.sleep(.1)
        raise AssertionError(f'popup {title!r} never appeared: {seen}')

    try:
        process, pid = desktop.start(exe)
        identity = command(cmd='magic_eq_identity')
        assert identity['pid'] == pid and Path(identity['exe_path']).resolve() == exe, identity
        assert Path(identity['isolation_root']).resolve() == root, identity
        verified = True
        report['identity'] = identity
        state = wait_state(driver, lambda s: s['main_ui_ready'], 'main UI ready', 90)
        if state.get('startup_needs_database_setup') or not state.get('db_path'):
            wait_state(driver, lambda s: s['new_db_dialog_open'], 'setup dialog open', 30)
            command(cmd='info_edit_input', action='new_db_name', text='AlertDialogQA' + uuid.uuid4().hex[:6])
            time.sleep(.2)
            command(cmd='info_edit_input', action='new_db_create')
            state = wait_state(driver, lambda s: bool(s.get('db_path')) and
                               not s.get('db_switch_in_progress') and
                               not s.get('startup_needs_database_setup'),
                               'empty library ready', 120)
        report['database'] = state['db_path']

        failures = []
        for dialog, locale_id, description in DIALOGS:
            title = UI_TEXT[locale_id]
            entry = {'dialog': dialog, 'title': title, 'description': description, 'themes': []}
            report['dialogs'].append(entry)
            for theme, theme_key, theme_name in THEMES:
                command(cmd='set_theme', theme=theme)
                time.sleep(.35)
                current = command(cmd='get_state')
                assert int(current.get('theme', -1)) == theme, current
                command(cmd='alert_dialog_preview', action='open', dialog=dialog)
                popup = popup_for(title)
                previous = popup['bounds']
                for _ in range(12):
                    time.sleep(.12)
                    candidate = popup_for(title)
                    if candidate['bounds'] == previous:
                        popup = candidate
                        break
                    previous = candidate['bounds']
                    popup = candidate
                time.sleep(.2)
                path = out / f'{dialog}-theme-{theme}.png'
                capture(driver, path)
                with Image.open(path) as source:
                    image = source.convert('RGB')
                    metrics = analyze(image, popup['bounds'], title)
                    crop_box = tuple(int(round(v)) for v in popup['bounds'])
                    image.crop(crop_box).save(out / f'{dialog}-theme-{theme}-dialog.png')
                checks = {
                    'rounded_corner': metrics['rounded_corner'],
                    'no_separator': not metrics['separator_rows'],
                    'title_visible': metrics['title_pixels'] >= 20,
                    'body_visible': metrics['text_pixels'] >= 300,
                    'readable_p95': metrics['contrast_p95'] >= 4.5,
                    'readable_max': metrics['contrast_max'] >= 4.5,
                }
                row = {'theme': theme, 'theme_key': theme_key, 'theme_name': theme_name,
                       'screenshot': path.name,
                       'dialog_crop': f'{dialog}-theme-{theme}-dialog.png',
                       'metrics': metrics, 'checks': checks, 'passed': all(checks.values())}
                if not row['passed']:
                    failures.append({'dialog': dialog, 'theme': theme,
                                     'failed': [k for k, value in checks.items() if not value]})
                entry['themes'].append(row)
                command(cmd='alert_dialog_preview', action='close')
                time.sleep(.2)

        report['failures'] = failures
        report['passed'] = not failures
        report['finished_at'] = datetime.now(timezone.utc).isoformat()
        save()
        write_html(report, out)
        return 0 if report['passed'] else 1
    except Exception as error:  # noqa: BLE001 - report fixture failures explicitly
        report['error'] = f'{type(error).__name__}: {error}'
        save()
        return 2
    finally:
        if process is not None:
            try:
                if win32event.WaitForSingleObject(process, 0) == win32con.WAIT_TIMEOUT:
                    if verified:
                        command(cmd='alert_dialog_preview', action='close')
                        command(cmd='info_edit_input', action='exit')
                    else:
                        import win32gui
                        win32gui.PostMessage(identity['hwnd'], win32con.WM_CLOSE, 0, 0)
                    if win32event.WaitForSingleObject(process, 15000) != win32con.WAIT_OBJECT_0:
                        win32process.TerminateProcess(process, 1)
                process.Close()
            except Exception as error:  # noqa: BLE001
                report['shutdown_error'] = f'{type(error).__name__}: {error}'
            process = None
        desktop.close()


def write_html(report, out):
    rows = []
    for entry in report['dialogs']:
        cards = []
        for item in entry['themes']:
            metrics = item['metrics']
            status = '\u901a\u8fc7' if item['passed'] else '\u5931\u8d25'
            cards.append(
                '<figure class="card">'
                f'<img src="{item["dialog_crop"]}" alt="{entry["title"]} - {item["theme_name"]}">'
                f'<figcaption><b>{item["theme_name"]}</b> · {status}<br>'
                f'\u5706\u89d2 {"OK" if item["checks"]["rounded_corner"] else "FAIL"} · '
                f'\u65e0\u5206\u5272\u7ebf {"OK" if item["checks"]["no_separator"] else "FAIL"} · '
                f'\u5bf9\u6bd4\u5ea6 P95 {metrics["contrast_p95"]:.1f} · \u6807\u9898\u50cf\u7d20 {metrics["title_pixels"]}</figcaption>'
                '</figure>')
        rows.append(f'<section><h2>{entry["title"]}</h2><p>{entry["description"]}</p>'
                    f'<div class="grid">{"".join(cards)}</div></section>')
    summary = '\u5168\u90e8\u901a\u8fc7' if report['passed'] else f'\u5931\u8d25 {len(report.get("failures", []))}'
    html_text = f'''<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><title>\u540c\u7c7b\u5c0f\u5f39\u7a97\u5706\u89d2\u5916\u89c2\u4e0e\u7cbe\u7b80\u6587\u6848 \u00b7 \u771f\u5b9e\u8fd0\u884c\u622a\u56fe</title>
<style>
body{{font-family:"Segoe UI","Microsoft YaHei",sans-serif;margin:24px;background:#f6f7f9;color:#1f2328}}
h1{{font-size:22px;margin:0 0 8px}}h2{{font-size:17px;margin:22px 0 4px}}
.meta{{color:#57606a;font-size:13px;line-height:1.6}}
.grid{{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:14px;margin-top:10px}}
.card{{margin:0;background:#fff;border:1px solid #d8dee4;border-radius:10px;padding:10px}}
.card img{{width:100%;height:auto;display:block;border-radius:6px;background:#eef1f4}}
figcaption{{font-size:12px;color:#57606a;margin-top:8px;line-height:1.5}}
.status{{font-weight:600;color:{'#1a7f37' if report['passed'] else '#cf222e'}}}
</style></head><body>
<h1>\u540c\u7c7b\u5c0f\u5f39\u7a97\u5706\u89d2\u5916\u89c2\u4e0e\u7cbe\u7b80\u6587\u6848 \u00b7 \u771f\u5b9e\u8fd0\u884c\u622a\u56fe</h1>
<p class="meta">QA \u5305: <code>{report.get('exe','')}</code><br>
SHA-256: <code>{report.get('sha256','')}</code><br>
\u9694\u79bb\u6839: <code>{report.get('isolation_root','')}</code><br>
\u8fd0\u884c\u65f6\u95f4: {report.get('started_at','')} - {report.get('finished_at','')}<br>
\u4e94\u4e2a\u4e3b\u9898 \u00d7 \u516d\u4e2a\u4ee3\u8868\u5f39\u7a97\uff1b\u6bcf\u5f20\u5361\u7247\u6838\u5bf9\u5706\u89d2\u3001\u65e0\u6b63\u6587\u5206\u5272\u7ebf\u3001\u6807\u9898\u4e0e\u6b63\u6587\u53ef\u89c1\u3001\u5bf9\u6bd4\u5ea6\u3002<br>
\u603b\u4f53\u7ed3\u679c: <span class="status">{summary}</span></p>
{''.join(rows)}
</body></html>'''
    (out / 'index.html').write_text(html_text, encoding='utf-8')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--isolation-root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    raise SystemExit(run(parser.parse_args()))


if __name__ == '__main__':
    main()
