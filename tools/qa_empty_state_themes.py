"""Capture five empty-database themes from a pinned isolated EXE; never use the mouse.

Exit 0: captured text checks pass; 1: contrast failure; 2: capture/identity failure.
Buttons' hover/active palette combinations are covered by TestEmptyStateContrast.
"""
import argparse
import ctypes
from ctypes import wintypes as w
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
from qa_transport_themes import Driver, THEMES, contrast, window_api


def verify_pixels(check, pixels):
    bg=pixels.most_common(1)[0][0]
    corners=check['backgrounds']
    if any(not min(c[i] for c in corners)-3 <= bg[i] <= max(c[i] for c in corners)+3 for i in range(3)):
        raise ValueError('Captured background does not match rendered theme')
    fg=check['fg'];alpha=fg[3]/255
    expected=[fg[i]*alpha+bg[i]*(1-alpha) for i in range(3)]
    ink={rgb:n for rgb,n in pixels.items() if max(abs(rgb[i]-expected[i]) for i in range(3))<=8}
    if sum(ink.values())<3:
        raise ValueError('No sufficient matching text pixels in screenshot')
    return max(contrast([*rgb,255],[*bg,255]) for rgb in ink)


def fresh(driver, theme):
    first = driver.command({'cmd': 'empty_state_audit'})
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        snap = driver.command({'cmd': 'empty_state_audit'})
        if (snap['theme'] == theme and snap['frame'] >= first['current_frame'] + 8
                and snap['current_frame'] - snap['frame'] <= 2
                and {c['id'] for c in snap['checks']} == {'title', 'hint', 'new', 'open'}):
            return snap
        time.sleep(.04)
    raise TimeoutError('No fresh setup frame with all four text elements')


def run(args):
    from PIL import Image, ImageGrab, ImageDraw, ImageFont
    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    if (runtime / 'qa_cmd.json').exists():
        raise RuntimeError('Pending QA command; refusing to overwrite')
    user32 = window_api()
    process = subprocess.Popen([str(exe)], cwd=exe.parent, env=dict(os.environ,
        AURA_HARNESS_MULTI_INSTANCE='1', AURA_HARNESS_LANE_KIND='functional', AURA_DISABLE_SINGLE_INSTANCE='1'))
    driver = Driver(runtime, out)
    rows, identity = [], {}
    try:
        identity = driver.command({'cmd': 'magic_eq_identity'}, timeout=40)
        if (identity['pid'] != process.pid or Path(identity['exe_path']).resolve() != exe
                or Path(identity['isolation_root']).resolve() != root):
            raise RuntimeError('Wrong EXE, PID or pinned profile')
        initial = driver.command({'cmd': 'get_state'})
        if initial['db_path']:
            raise RuntimeError('Profile already has a database; refusing to change it')
        identity.update(exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
            captured_at=datetime.now(timezone.utc).isoformat(),
            git_revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=Path(__file__).resolve().parents[1],text=True).strip())
        source_root=Path(__file__).resolve().parents[1]
        identity['worktree_dirty']=bool(subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=source_root,text=True).strip())
        identity['source_sha256']={p:hashlib.sha256((source_root/p).read_bytes()).hexdigest() for p in
            ('src/main.cpp','src/WindowsMain.cpp','src/BuiltinThemeScheme.cpp','src/ui/MistBlueChrome.h','src/ui/EmptyStatePalette.h','src/ui/EmptyStateVisualAudit.h','tools/qa_empty_state_themes.py')}
        for theme, name in THEMES:
            driver.command({'cmd': 'set_theme', 'theme': theme})
            snap = fresh(driver, theme)
            pid = w.DWORD(); user32.GetWindowThreadProcessId(identity['hwnd'], ctypes.byref(pid))
            if pid.value != process.pid: raise RuntimeError('Window owner changed')
            point, rect = w.POINT(0,0), w.RECT()
            user32.ClientToScreen(identity['hwnd'],ctypes.byref(point))
            user32.GetClientRect(identity['hwnd'],ctypes.byref(rect))
            shot = ImageGrab.grab(window=identity['hwnd']).convert('RGB')
            if shot.size != (rect.right,rect.bottom): raise RuntimeError('Client size mismatch')
            def crop_box(bounds):
                box=tuple(round(v-(point.x if i%2==0 else point.y)) for i,v in enumerate(bounds))
                if box[0]<0 or box[1]<0 or box[2]>shot.width or box[3]>shot.height:
                    raise RuntimeError('Measured region outside captured client')
                return box
            panel = shot.crop(crop_box(snap['panel']))
            if len(panel.getcolors(panel.width*panel.height) or [])<8: raise RuntimeError('Blank screenshot')
            checks=[]
            for c in snap['checks']:
                region=shot.crop(crop_box(c['bounds']))
                pixels=Counter(region.get_flattened_data() if hasattr(region,'get_flattened_data') else region.getdata())
                strongest=verify_pixels(c,pixels)
                ratio=min(contrast(c['fg'],b) for b in c['backgrounds'])
                panel_backgrounds=next(item['backgrounds'] for item in snap['checks'] if item['id']=='title')
                surface_ratio=min(contrast(a,b) for a,b in zip(c['backgrounds'],panel_backgrounds)) if c['id']=='open' else None
                checks.append(dict(c, ratio=ratio, pixel_max_contrast=strongest,
                    button_surface_ratio=surface_ratio,
                    status='pass' if min(ratio,strongest)>=4.5 and (surface_ratio is None or surface_ratio>=3) else 'fail'))
            after=driver.command({'cmd':'empty_state_audit'})
            if after['theme']!=theme or after['frame']<snap['frame']: raise RuntimeError('Theme changed during capture')
            if after['checks']!=snap['checks'] or after['panel']!=snap['panel']:
                raise RuntimeError('Colors or layout changed during capture; retry with a stable window')
            full=out/f'{theme}-full.png'; cropped=out/f'{theme}-panel.png'
            shot.save(full);panel.save(cropped)
            rows.append(dict(theme=theme,name=name,checks=checks,snapshot=snap,client_size=shot.size,
                full_sha256=hashlib.sha256(full.read_bytes()).hexdigest(),panel_sha256=hashlib.sha256(cropped.read_bytes()).hexdigest()))
        driver.command({'cmd':'set_theme','theme':initial['theme']})
        (out/'report.json').write_text(json.dumps(dict(identity=identity,results=rows),ensure_ascii=False,indent=2),encoding='utf-8')
        images=[Image.open(out/f'{r["theme"]}-panel.png') for r in rows]
        canvas=Image.new('RGB',(max(im.width for im in images)+40,sum(im.height+45 for im in images)), '#eeeeee')
        draw=ImageDraw.Draw(canvas);font=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',18);y=0
        for r,im in zip(rows,images):
            draw.text((20,y+4),r['name'],font=font,fill='#222222');canvas.paste(im,(20,y+30));y+=im.height+45
        canvas.save(out/'comparison.png')
        for r in rows: print(r['name'],{c['id']:round(c['ratio'],2) for c in r['checks']})
        return int(any(c['status']!='pass' for r in rows for c in r['checks']))
    finally:
        if process.poll() is None:
            if identity.get('pid')==process.pid: user32.PostMessageW(identity['hwnd'],0x0010,0,0)
            try: process.wait(timeout=10)
            except subprocess.TimeoutExpired: process.terminate();process.wait(timeout=5)


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('exe','isolation-root','output'): parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args()
    try: raise SystemExit(run(args))
    except Exception as error:
        print(str(error));raise SystemExit(2)
