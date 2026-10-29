"""Check cover-wall stability in a disposable, compile-time isolated QA package.

Requires the local-only qa_cover_trace/qa_cover_scroll framebuffer supplement.
The trace records consecutive rendered frames, avoiding polling/parity aliasing.
No shared desktop input is used. A prepared 48-cover fixture is required.
"""
from __future__ import annotations
import argparse, ctypes, hashlib, json, os, re, subprocess, time, uuid
from pathlib import Path
from PIL import Image
from qa_transport_themes import Driver, THEMES, window_api


def audit_pixels(output, runtime):
    """Compare rendered grid pixels, excluding the transient scrollbar and slider."""
    report=json.loads((output/'report.json').read_text('utf-8')); results=[]
    for case in report['cases']:
        if 'screenshot' not in case: continue
        target=Image.open(case['screenshot']).convert('RGB')
        raw=Image.open(runtime/(case['trace_key']+'-0.ppm')).convert('RGB')
        w,h=target.size; left=raw.width-w
        row=target.crop((0,0,w,1)).tobytes(); expected=target.tobytes()
        # Locate the exact saved Info crop instead of assuming Windows border DPI.
        found=[y for y in range(raw.height-h+1)
            if raw.crop((left,y,left+w,y+1)).tobytes()==row
            and raw.crop((left,y,left+w,y+h)).tobytes()==expected]
        assert len(found)==1,(case['name'],found)
        y=found[0]; box=(left+14,y+43,left+w-28,y+h-45)
        hashes=[]
        for i in range(24):
            image=Image.open(runtime/(case['trace_key']+f'-{i}.ppm')).convert('RGB').crop(box)
            hashes.append(hashlib.sha256(image.tobytes()).hexdigest())
        changes=sum(a!=b for a,b in zip(hashes,hashes[1:]))
        results.append(dict(name=case['name'],frames=24,pixel_changes=changes,hashes=hashes,grid_crop=box))
    passed=bool(results) and all(r['pixel_changes']==0 for r in results)
    (output/'pixel-audit.json').write_text(json.dumps(dict(passed=passed,cases=results),indent=2),encoding='utf-8')
    print(f'Pixel audit: {len(results)} scenes; {sum(r["pixel_changes"] for r in results)} changes',flush=True)
    assert passed,'Cover pixels changed between stationary frames'


def run(args):
    exe, root, output = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    runtime = root/'profile/runtime'
    driver, windows = Driver(runtime, output), window_api()
    cases, identity = [], {}
    process = None
    def command(**kw): return driver.command(kw)
    def settled():
        first = command(cmd='dock_state'); end = time.monotonic()+20
        while time.monotonic() < end:
            state = command(cmd='dock_state')
            if state['frame'] >= first['frame']+8: return state
            time.sleep(.03)
        raise AssertionError('UI frames stopped')
    def resize(height):
        state = settled(); panel = state['panels']['info']
        direction = 1 if state['layout'][2] == 'info' else -1
        # Cross the drag threshold even for a two-pixel boundary adjustment.
        for phase in ('hover','down'):
            point = command(cmd='dock_pointer',phase=phase,module='info',part='band-splitter'); settled()
        command(cmd='dock_pointer',phase='move',x=point['x'],y=point['y']+20*direction); settled()
        y = point['y']+(height-panel['h'])*direction
        command(cmd='dock_pointer',phase='move',x=point['x'],y=y); settled()
        command(cmd='dock_pointer',phase='up',x=point['x'],y=y)
        state = settled()
        assert abs(state['panels']['info']['h']-height) <= 2, state
    def ready(size):
        command(cmd='set_cover_wall_thumb_size',size=size)
        end = time.monotonic()+30
        while time.monotonic()<end:
            settled(); wall = command(cmd='get_state')['cover_wall']
            # Theme switches can rebuild textures while the temporary display
            # snapshot is already "ready". Require the complete known fixture.
            if (wall['display_count'] == 48 and wall['visible_tiles'] > 0
                    and wall['visible_ready'] == wall['visible_tiles']
                    and wall['pending_cover_work'] == 0 and not wall['loading']): return
        raise AssertionError(wall)
    def capture(name, images=False):
        state = settled(); key = uuid.uuid4().hex
        command(cmd='qa_cover_trace',capture_key=key,frames=args.frames,images=images)
        path = runtime/(key+'.jsonl'); end = time.monotonic()+30
        while True:
            try: rows = [json.loads(x) for x in path.read_text('utf-8').splitlines()]
            except (FileNotFoundError, json.JSONDecodeError): rows = []
            if len(rows) == args.frames: break
            assert time.monotonic()<end, 'Incomplete frame trace'
            time.sleep(.08)
        (output/(name+'.json')).write_text(json.dumps(rows,indent=2),encoding='utf-8')
        assert all(b['frame']==a['frame']+1 for a,b in zip(rows,rows[1:])), 'Frame gaps'
        fields = ('columns','thumb','scrollbar','content_height','visible','ready','placeholders','items','generation','scroll_y')
        changes = [i for i in range(1,len(rows)) if any(rows[i][k]!=rows[i-1][k] for k in fields)]
        case = dict(name=name,frames=len(rows),changes=len(changes),passed=not changes,
                    first=rows[0],last=rows[-1],state=state,trace_key=key)
        if images:
            from ctypes import wintypes
            point = wintypes.POINT(0,0); windows.ClientToScreen(hwnd,ctypes.byref(point))
            panel = state['panels']['info']
            box = (int(panel['x']-point.x),int(panel['y']-point.y),
                   int(panel['x']-point.x+panel['w']),int(panel['y']-point.y+panel['h']))
            frames = [Image.open(runtime/(key+f'-{i}.ppm')).convert('RGB').crop(box) for i in range(24)]
            frames[0].save(output/(name+'.png'))
            # Preserve actual consecutive images; slower playback makes small jitter visible.
            frames[0].save(output/(name+'.gif'),save_all=True,append_images=frames[1:],duration=100,loop=0)
            case['screenshot'] = str(output/(name+'.png'))
        cases.append(case)
        print(('PASS' if case['passed'] else 'FAIL')+': '+name+f' ({len(rows)} frames; {len(changes)} changes)',flush=True)
        assert not changes, f'{name}: stationary cover wall changed on {len(changes)} of {len(rows)-1} frame transitions'
        assert rows[-1]['ready']>0 and rows[-1]['placeholders']==0, rows[-1]
    def launch(side):
        nonlocal process, identity
        startup = subprocess.STARTUPINFO(); startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow = 4
        process = subprocess.Popen([str(exe)],cwd=exe.parent,startupinfo=startup,
            env=dict(os.environ,AURA_HARNESS_MULTI_INSTANCE='1',AURA_HARNESS_LANE_KIND='functional',AURA_DISABLE_SINGLE_INSTANCE='1'))
        identity = driver.command({'cmd':'magic_eq_identity'},timeout=60)
        assert identity['pid']==process.pid and Path(identity['exe_path']).resolve()==exe, identity
        assert Path(identity['isolation_root']).resolve()==root, identity
        hwnd = identity['hwnd']; windows.ShowWindow(hwnd,4)
        end = time.monotonic()+45
        while not command(cmd='magic_eq_state')['main_ui_ready']:
            assert time.monotonic()<end; time.sleep(.1)
        windows.SetWindowPos(hwnd,None,40,40,1120,1600,0x0014)
        command(cmd='qa_narration',action='close'); command(cmd='dock_pointer',phase='focus'); settled()
        assert settled()['layout'][side]=='info'
        for phase in ('hover','down','up'):
            command(cmd='info_page_pointer',key='info.artwork',phase=phase); settled()
        return hwnd
    settings = root/'profile/Local/HarkenSearch/settings.ini'
    original = settings.read_text('utf-8-sig')
    (output/'settings-before.ini').write_text(original,encoding='utf-8')
    failure = None
    try:
        for side in ((2,) if args.boundary_only else (2,3)):
            fixture = original
            values = dict(panelFileInfo_floating=0,panelFileInfo_dockSide=side,fileInfoDockSide=side,
                panelFileInfo_dockedHeight=540,panelSidebar_dockSide=0,sidebarDockSide=0,
                panelSidebar_floating=0,panelSidebar_dockedWidth=250,panelSimilarSounds_visible=0,infoSelectedPage=1)
            for key,value in values.items():
                fixture,n = re.subn('^'+key+'=.*$',key+'='+str(value),fixture,flags=re.M)
                if not n: fixture+='\n'+key+'='+str(value)
            settings.write_text(fixture,encoding='utf-8')
            hwnd = launch(side)
            ready(56); resize(362)
            capture(f'side-{side}-original-boundary',images=True)
            if not args.boundary_only:
                for theme,_ in THEMES:
                    command(cmd='set_theme',theme=theme); settled()
                    for height in (358,362,366):
                        resize(height); ready(56)
                        capture(f'side-{side}-theme-{theme}-height-{height}',images=height==362)
                command(cmd='set_theme',theme=2); resize(220); ready(56)
                command(cmd='qa_cover_scroll',ratio=1.0); settled(); ready(56)
                capture(f'side-{side}-scrolled-bottom',images=True)
                assert cases[-1]['last']['scroll_y']>0,cases[-1]
                command(cmd='qa_cover_scroll',ratio=0.0); settled(); ready(56)
                capture(f'side-{side}-scrolled-top')
                resize(680); ready(132); capture(f'side-{side}-large-covers',images=True)
                ready(56); capture(f'side-{side}-dense-covers')
                windows.SetWindowPos(hwnd,None,40,40,1100,900,0x0014); settled(); ready(56)
                capture(f'side-{side}-shrink-window')
                windows.SetWindowPos(hwnd,None,40,40,1120,1600,0x0014); settled(); ready(56)
                capture(f'side-{side}-restore-window')
            command(cmd='info_edit_input',action='exit'); process.wait(timeout=20)
    except Exception as error:
        failure = str(error)
        raise
    finally:
        if process and process.poll() is None:
            try: command(cmd='info_edit_input',action='exit'); process.wait(timeout=20)
            except (RuntimeError,TimeoutError,subprocess.TimeoutExpired): process.terminate(); process.wait(timeout=5)
        settings.write_text(original,encoding='utf-8')
        report = dict(passed=failure is None,error=failure,cases=cases,identity=identity,
            exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),input_method='app-owned; no desktop cursor',
            capture_method='consecutive OpenGL frames; local-only QA supplement')
        (output/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    audit_pixels(output, runtime)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',type=Path,required=True)
    parser.add_argument('--isolation-root',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--frames',type=int,default=180)
    parser.add_argument('--boundary-only',action='store_true')
    parser.add_argument('--audit-only',action='store_true',help='Check saved consecutive frame pixels without launching the app')
    args=parser.parse_args()
    if args.audit_only: audit_pixels(args.output,args.isolation_root/'profile/runtime')
    else: run(args)
