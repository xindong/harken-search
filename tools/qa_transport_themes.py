"""Audit actual transport draw colors and capture five themes in an isolated QA EXE.

Exit 0: measured scenarios pass; 1: readability/coverage failures; 2: capture error.
Requires Pillow for screenshots. Never attaches to the installed application.
"""
from __future__ import annotations

import math
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time
import uuid
import wave
from datetime import datetime, timezone


def _rgba(color):
    if len(color) != 4 or any(not math.isfinite(x) or not 0 <= x <= 255 for x in color):
        raise ValueError('Expected four finite RGBA channels in [0,255]')
    return [x / 255 for x in color]


def contrast(foreground, background):
    f, b = _rgba(foreground), _rgba(background)
    if b[3] != 1:
        raise ValueError('Background must include every underlying layer')
    rgb = [f[i] * f[3] + b[i] * (1 - f[3]) for i in range(3)]
    def luminance(channels):
        return sum(w * (c / 12.92 if c <= .04045 else ((c + .055) / 1.055) ** 2.4)
                   for w, c in zip((.2126, .7152, .0722), channels))
    lo, hi = sorted((luminance(rgb), luminance(b[:3])))
    return (hi + .05) / (lo + .05)


def classify(ratio, kind, disabled):
    if kind not in ('text', 'graphic') or not math.isfinite(ratio):
        raise ValueError('Invalid contrast observation')
    if disabled:
        return 'disabled'
    return 'pass' if ratio >= (4.5 if kind == 'text' else 3.0) else 'fail'


REQUIRED = {'timecode', 'volume.label', 'stereo', 'reverse', 'pan.arc', 'pan.dot',
            'center.outline', 'bass_mono', 'eq.label', 'eq.frequency', 'eq.grip',
            'eq.mode', 'database.label', 'spectrum.label', 'count'}


def required_for(scenario, snapshot):
    if snapshot['spectrum'] != (scenario == 'spectrum') or snapshot['selection'] != (scenario == 'selection'):
        raise ValueError('Rendered controls do not match the requested scenario')
    required = REQUIRED | {'volume.outline', 'database.arrow', 'spectrum.box', 'eq.grip.right', 'eq.center', 'eq.slope'}
    if snapshot['spectrum']:
        required |= {'spectrum.check'}
        if snapshot['spectrum_collapse'] <= 1:
            required |= {'spectrum.blend', 'spectrum.blend.grip'}
        if snapshot['spectrum_collapse'] == 0:
            required |= {'spectrum.fft', 'spectrum.fft.arrow', 'spectrum.split', 'spectrum.split.box'}
        else:
            required |= {'spectrum.overflow'}
    if snapshot['selection']:
        required |= {'selection.export'}
    if scenario == 'processing':
        required |= {'pan.value'}
    if scenario == 'wide':
        required |= {'eq.tick_label'}
    return required


def analyze_snapshot(snapshot, required=None):
    required = REQUIRED if required is None else required
    bar = snapshot['bar']
    checks = []
    for original in snapshot['checks']:
        row = dict(original)
        row['ratio'] = contrast(row['fg'], row['bg'])
        row['minimum'] = 4.5 if row['kind'] == 'text' else 3.0
        row['status'] = classify(row['ratio'], row['kind'], row['disabled'])
        row['text_clipping_measured'] = row.get('bounds_kind') == 'glyphs' and 'clip' in row
        x0, y0, x1, y1 = row['bounds']
        clip = row.get('clip', bar)
        limits = [max(bar[0], clip[0]), max(bar[1], clip[1]), min(bar[2], clip[2]), min(bar[3], clip[3])]
        if x0 < limits[0] - .5 or y0 < limits[1] - .5 or x1 > limits[2] + .5 or y1 > limits[3] + .5 or x1 <= x0 or y1 <= y0:
            row['status'] = 'clipped'
        checks.append(row)
    return {'checks': checks, 'missing': sorted(required - {x['id'] for x in checks})}


THEMES = [(0, '经典深色'), (1, '极光青'), (2, '深海军蓝'), (3, '海玻璃'), (10, '柔雾粉')]
SCENARIOS = ['pass', 'bypass', 'notch', 'processing', 'mono', 'spectrum', 'overdrive', 'fill-boundary', 'selection', 'wide']


class Driver:
    def __init__(self, runtime, output):
        self.runtime, self.output = runtime, output

    def command(self, payload, timeout=25):
        request = dict(payload, request_id=uuid.uuid4().hex)
        target = self.runtime / 'qa_cmd.json'
        if target.exists():
            raise RuntimeError('Another command is pending in this isolation root')
        tmp = target.with_suffix('.' + request['request_id'] + '.tmp')
        tmp.write_text(json.dumps(request, ensure_ascii=False), encoding='utf-8')
        tmp.replace(target)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                result = json.loads((self.runtime / 'qa_result.json').read_text(encoding='utf-8'))
                if result.get('request_id') == request['request_id']:
                    with (self.output / 'commands.jsonl').open('a', encoding='utf-8') as f:
                        f.write(json.dumps({'request': request, 'result': result}, ensure_ascii=False) + '\n')
                    if not result.get('ok'):
                        raise RuntimeError(result)
                    return result
            except (FileNotFoundError, PermissionError, json.JSONDecodeError):
                pass
            time.sleep(.04)
        raise TimeoutError(payload)

    def rendered(self, theme):
        first = self.command({'cmd': 'transport_audit'})
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            snap = self.command({'cmd': 'transport_audit'})
            if (snap['theme'] == theme and snap['frame'] >= first['current_frame'] + 8
                    and snap['current_frame'] - snap['frame'] <= 2 and snap['checks']):
                return snap
            time.sleep(.05)
        raise TimeoutError('Theme did not produce a fresh transport frame')


def synthetic_audio(path):
    if path.exists():
        return
    with wave.open(str(path), 'wb') as f:
        f.setparams((2, 2, 48000, 0, 'NONE', 'not compressed'))
        block = b''.join(struct.pack('<hh', int(300 * math.sin(i * 2 * math.pi * 440 / 48000)),
                                     int(300 * math.sin(i * 2 * math.pi * 660 / 48000))) for i in range(48000))
        f.writeframes(block * 8)


def window_api():
    import ctypes
    from ctypes import wintypes as w
    u = ctypes.WinDLL('user32', use_last_error=True)
    u.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    u.GetClientRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
    u.ClientToScreen.argtypes = [w.HWND, ctypes.POINTER(w.POINT)]
    u.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
    u.ShowWindow.argtypes = [w.HWND, ctypes.c_int]
    u.SetForegroundWindow.argtypes = [w.HWND]
    u.SetWindowPos.argtypes = [w.HWND, w.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, w.UINT]
    u.GetForegroundWindow.restype = w.HWND
    u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
    return u


def capture(user32, hwnd, pid, snapshot, destination):
    import ctypes
    from ctypes import wintypes as w
    from PIL import ImageGrab
    actual_pid = w.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(actual_pid))
    if actual_pid.value != pid:
        raise RuntimeError('QA window identity changed; screenshot rejected')
    rect, point = w.RECT(), w.POINT(0, 0)
    if not user32.GetClientRect(hwnd, ctypes.byref(rect)) or not user32.ClientToScreen(hwnd, ctypes.byref(point)):
        raise OSError('Cannot read QA client coordinates')
    bounds = (point.x, point.y, point.x + rect.right, point.y + rect.bottom)
    image = ImageGrab.grab(window=hwnd)
    if image.size != (rect.right, rect.bottom):
        raise RuntimeError(f'Window capture size {image.size} does not match client {(rect.right, rect.bottom)}')
    image.save(destination.with_suffix('.png'))
    bar = snapshot['bar']
    crop = tuple(round(v - (point.x if i % 2 == 0 else point.y)) for i, v in enumerate(bar))
    if crop[0] < 0 or crop[1] < 0 or crop[2] > image.width or crop[3] > image.height:
        raise RuntimeError('Transport crop is outside actual client image')
    cropped = image.crop(crop)
    if len(cropped.getcolors(cropped.width * cropped.height) or []) < 8:
        raise RuntimeError('Empty or unrendered screenshot')
    cropped.save(destination.with_name(destination.name + '-bar.png'))
    surface = tuple(snapshot['surface'][:3])
    matching = sum(n for n, rgb in cropped.convert('RGB').getcolors(cropped.width * cropped.height) or []
                   if max(abs(rgb[i] - surface[i]) for i in range(3)) <= 2)
    if matching / (cropped.width * cropped.height) < .15:
        raise RuntimeError('Screenshot does not contain the rendered transport surface')
    return {'client_screen_bounds': bounds, 'bar_client_bounds': crop, 'client_size': image.size,
            'surface_match_fraction': matching / (cropped.width * cropped.height),
            'full_png_sha256': hashlib.sha256(destination.with_suffix('.png').read_bytes()).hexdigest(),
            'bar_png_sha256': hashlib.sha256(destination.with_name(destination.name + '-bar.png').read_bytes()).hexdigest()}


def reports(output, results, identity):
    from PIL import Image, ImageDraw, ImageFont
    (output / 'report.json').write_text(json.dumps({'identity': identity, 'results': results},
        ensure_ascii=False, indent=2), encoding='utf-8')
    rows = ['# 播放栏五主题巡检', '', '本报告检查真实程序绘制颜色；普通文字 ≥4.5，必要图形 ≥3。',
            '真正禁用的项目单列；本报告不等于人工视觉验收。',
            '范围：音效模式顶部播放栏，包含频谱附加控件、选区导出和宽 EQ。',
            '未覆盖：音乐模式、弹窗、悬停/拖动、波形底栏、变调轮、扫描中状态及其他窗口尺寸。',
            '每个状态的检测点分别计数；装饰性分隔线不作为必要控件。',
            '裁切只覆盖已登记的区域：EQ 文字使用真实字形及绘制时裁切区；其它文字若只登记控件框，其内部裁切单列为未测，不能据此认定文字完整。',
            f"程序 SHA-256：{identity.get('exe_sha256', '')}",
            f"代码版本：{identity.get('git_revision', identity.get('code_revision', ''))}",
            '', '| 主题 | 状态 | 对比度异常 | 已测区域裁切 | 缺失 | 禁用 | 文字内部裁切未测 |',
            '|---|---|---:|---:|---:|---:|---:|']
    for r in results:
        count = lambda status: sum(x['status'] == status for x in r['checks'])
        unmeasured = sum(x['kind'] == 'text' and not x['text_clipping_measured'] for x in r['checks'])
        rows.append(f"| {r['name']} | {r['scenario']} | {count('fail')} | {count('clipped')} | {len(r['missing'])} | {count('disabled')} | {unmeasured} |")
    rows += ['', '## 异常明细', '']
    for r in results:
        bad = [x for x in r['checks'] if x['status'] in ('fail', 'clipped')]
        if bad or r['missing']:
            rows += [f"### {r['name']} / {r['scenario']}", '', f"![真实播放栏]({r['stem']}-bar.png)", '']
            rows += [f"- {x['id']}：{x['ratio']:.2f}:1，要求 {x['minimum']}:1，{x['status']}" for x in bad]
            rows += [f"- 未采集：{x}" for x in r['missing']]
            rows.append('')
    (output / '异常汇总.md').write_text('\n'.join(rows), encoding='utf-8')
    panels = [(r, Image.open(output / (r['stem'] + '-bar.png')).convert('RGB'))
              for r in results if r['scenario'] == 'pass']
    font = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 19)
    width = max(im.width for _, im in panels)
    sheet = Image.new('RGB', (width, sum(im.height + 38 for _, im in panels)), '#e9edf0')
    draw = ImageDraw.Draw(sheet); y = 0
    for r, im in panels:
        draw.text((8, y + 4), r['name'], fill='#18222b', font=font)
        sheet.paste(im, (0, y + 34)); y += im.height + 38
    sheet.save(output / '五主题播放栏.png')


def run(args):
    if sys.platform != 'win32':
        raise RuntimeError('Real window capture requires Windows')
    exe, isolation, output = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    if not exe.is_file():
        raise FileNotFoundError(exe)
    output.mkdir(parents=True, exist_ok=False)
    runtime = isolation / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    if (runtime / 'qa_cmd.json').exists():
        raise RuntimeError('Pending command in isolation root; not overwriting it')
    fixture = isolation / 'transport-audit-tone.wav'
    synthetic_audio(fixture)
    user32 = window_api()
    env = dict(os.environ, AURA_HARNESS_MULTI_INSTANCE='1', AURA_HARNESS_LANE_KIND='functional',
               AURA_DISABLE_SINGLE_INSTANCE='1')
    process = subprocess.Popen([str(exe)], cwd=exe.parent, env=env)
    driver = Driver(runtime, output); hwnd = None
    results = []
    try:
        deadline = time.monotonic() + 40
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f'QA process exited: {process.returncode}')
            if (runtime / 'qa_result.json').exists() or time.monotonic() > deadline - 38:
                break
            time.sleep(.05)
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=40)
        if identity['pid'] != process.pid or Path(identity['exe_path']).resolve() != exe or Path(identity['isolation_root']).resolve() != isolation:
            raise RuntimeError('Executable, PID or pinned isolation root does not match')
        hwnd = identity['hwnd']
        user32.ShowWindow(hwnd, 4)
        width = min(args.width, user32.GetSystemMetrics(0) - 60)
        height = min(1000, user32.GetSystemMetrics(1) - 100)
        user32.SetWindowPos(hwnd, None, 20, 30, width, height, 0x0040 | 0x0010 | 0x0004)
        driver.command({'cmd': 'load_path', 'path': str(fixture)})
        deadline = time.monotonic() + 30
        while not driver.command({'cmd': 'get_state'}).get('audio_loaded'):
            if time.monotonic() > deadline:
                raise TimeoutError('Synthetic audio did not load')
            time.sleep(.1)
        identity.update(exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), code_revision=args.code_revision,
                        scenario_scope=SCENARIOS, fixture=str(fixture), requested_width=width,
                        coverage_scope='top transport, standard sound-effects mode',
                        not_covered=['music mode', 'popups', 'hover/drag', 'waveform footer', 'pitch wheel', 'active library scan'])
        source_root = Path(__file__).resolve().parents[1]
        source_paths = ['src/main.cpp', 'src/WindowsMain.cpp', 'src/ui/TransportContrast.h',
                        'src/BuiltinThemeScheme.cpp', 'src/ui/MistBlueChrome.h',
                        'src/ui/TransportVisualAudit.h', 'src/ui/TransportBlendSlider.h', 'src/ui/TransportValueText.h',
                        'tools/qa_transport_themes.py']
        identity['source_sha256'] = {p: hashlib.sha256((source_root / p).read_bytes()).hexdigest() for p in source_paths}
        identity['git_revision'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source_root, text=True).strip()
        identity['worktree_dirty'] = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=source_root, text=True).strip())
        identity['captured_at'] = datetime.now(timezone.utc).isoformat()
        for theme, name in THEMES:
            driver.command({'cmd': 'set_theme', 'theme': theme})
            for scenario in SCENARIOS:
                driver.command({'cmd': 'transport_audit_setup', 'scenario': scenario})
                snap = driver.rendered(theme)
                stem = f'{theme}-{scenario}'
                geometry = capture(user32, hwnd, process.pid, snap, output / stem)
                check = driver.command({'cmd': 'transport_audit'})
                if check['theme'] != theme or check['frame'] < snap['frame']:
                    raise RuntimeError('Theme changed during capture')
                (output / (stem + '.json')).write_text(json.dumps(snap, ensure_ascii=False, indent=2), encoding='utf-8')
                result = dict(analyze_snapshot(snap, required_for(scenario, snap)), theme=theme, name=name, scenario=scenario, stem=stem,
                              frame=snap['frame'], capture=geometry)
                results.append(result)
                print(json.dumps({'theme': name, 'scenario': scenario,
                    'failures': sum(x['status'] in ('fail', 'clipped') for x in result['checks']),
                    'missing': result['missing']}, ensure_ascii=False), flush=True)
        reports(output, results, identity)
        return int(any(r['missing'] or any(c['status'] in ('fail', 'clipped') for c in r['checks']) for r in results))
    except Exception as error:
        (output / 'capture-error.json').write_text(json.dumps({
            'status': 'incomplete', 'error': str(error), 'completed_scenarios': len(results),
            'expected_scenarios': len(THEMES) * len(SCENARIOS), 'results': results
        }, ensure_ascii=False, indent=2), encoding='utf-8')
        raise
    finally:
        if hwnd and process.poll() is None:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=5)
        elif process.poll() is None:
            process.terminate()
            process.wait(timeout=5)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--isolation-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--code-revision', required=True)
    parser.add_argument('--width', type=int, default=1920)
    try:
        sys.exit(run(parser.parse_args()))
    except Exception as error:
        print(f'巡检未完成：{error}', file=sys.stderr)
        sys.exit(2)
