"""Exercise the real Info UI in a compile-time isolated QA EXE without OS input."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
from PIL import ImageGrab
from qa_transport_themes import Driver, window_api


def run(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    exe = args.exe.resolve()
    root = args.isolation_root.resolve()
    runtime = root / 'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    driver = Driver(runtime, output)
    user32 = window_api()
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 4  # show without activating the user's desktop
    process = subprocess.Popen([str(exe)], cwd=exe.parent, startupinfo=startup,
        env=dict(os.environ, AURA_HARNESS_MULTI_INSTANCE='1',
                 AURA_HARNESS_LANE_KIND='functional', AURA_DISABLE_SINGLE_INSTANCE='1'))
    hwnd = None
    checks = []
    def command(**kw): return driver.command(kw)
    def state(): return command(cmd='info_page_state')
    def settled():
        first = state()
        end = time.monotonic() + 15
        while time.monotonic() < end:
            current = state()
            if current['frame'] >= first['frame'] + 3 and (current['zones'] or not current.get('visible',True)): return current
            time.sleep(.08)
        raise AssertionError(current)
    def click(key):
        assert key in settled()['zones'], key
        for phase in ['hover', 'down', 'up']:
            command(cmd='info_page_pointer', key=key, phase=phase)
            time.sleep(.12)
        return settled()
    def capture(name, key=None):
        snap=settled()
        capture_hwnd=hwnd
        if key:
            import ctypes
            from ctypes import wintypes as w
            zone=snap["zones"][key]
            x,y=zone["x"]+zone["w"]/2,zone["y"]+zone["h"]/2
            candidates=[]
            user32.GetWindowRect.argtypes=[w.HWND,ctypes.POINTER(w.RECT)]
            user32.IsWindowVisible.argtypes=[w.HWND]
            callback=ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
            @callback
            def visit(window,_):
                pid=w.DWORD(); rect=w.RECT()
                user32.GetWindowThreadProcessId(window,ctypes.byref(pid))
                if pid.value==process.pid and user32.IsWindowVisible(window):
                    user32.GetWindowRect(window,ctypes.byref(rect))
                    if rect.left<=x<rect.right and rect.top<=y<rect.bottom:
                        candidates.append(((rect.right-rect.left)*(rect.bottom-rect.top),window))
                return True
            user32.EnumWindows(visit,0)
            assert candidates, (key,snap)
            capture_hwnd=min(candidates)[1]
        shot = ImageGrab.grab(window=capture_hwnd)
        assert len(shot.convert('RGB').getcolors(shot.width * shot.height) or []) > 20
        shot.save(output / (name + '.png'))
    try:
        identity = driver.command({'cmd':'magic_eq_identity'}, timeout=50)
        assert identity['pid'] == process.pid and Path(identity['exe_path']).resolve() == exe
        assert Path(identity['isolation_root']).resolve() == root
        hwnd = identity['hwnd']
        user32.SetWindowPos(hwnd,None,40,40,1400,900,0x0014)
        deadline = time.monotonic() + 40
        while not command(cmd="magic_eq_state").get("main_ui_ready"):
            if time.monotonic() > deadline: raise TimeoutError("Main UI did not become ready")
            time.sleep(.2)
        user32.SetWindowPos(hwnd,None,40,40,1400,900,0x0014)
        try: command(cmd='qa_narration',action='close')
        except RuntimeError as error:
            if 'unknown command' not in str(error): raise
        initial=settled()
        checks.append({'case':'startup','state':initial})
        if args.edit_suite or args.edit_tray_suite or args.edit_update_suite or args.edit_new_db_suite:
            from qa_info_edit import run_edit_suite
            run_edit_suite(args, command, settled, click, capture, process, checks)
        elif args.expect_saved or args.prepare_demo:
            if args.expect_saved:
                assert initial['selected']==3 and initial['visible_mask']==11, initial
                capture('reopened')
            if args.prepare_demo:
                command(cmd="info_page_layout", floating=False, width=360)
                settled()
                command(cmd="info_page_layout", floating=True, width=360)
                for i in range(4):
                    if not settled()["visible_mask"] & (1 << i):
                        click("info.pageVisibility"); click("info.visibility."+str(i))
                current=click("info.artwork")
                assert current["visible_mask"]==15 and current["selected"]==1, current
                assert not current["outer_scrollbar"] and current["outer_scroll_max"]==0, current
                checks.append({"case":"ready-for-user","state":current})
                capture("ready-for-user", "info.artwork")
        else:
            for i in range(4):
                if not initial["visible_mask"] & (1<<i):
                    click("info.pageVisibility"); click("info.visibility."+str(i))
            keys=['info.metadataSummary','info.artwork','info.transferHistory','info.playHistory']
            for i,key in enumerate(keys):
                current=click(key)
                assert current['selected']==i, current
                checks.append({'case':'switch-'+str(i),'state':current})
                capture("page-"+str(i))
            click('info.artwork'); capture('artwork')
            click('info.pageVisibility'); capture('visibility-menu', "info.visibility.0")
            current=click('info.visibility.1')
            assert current['selected']==0 and current['visible_mask']==13, current
            assert 'info.artwork' not in current['zones'], current
            click('info.pageVisibility'); click('info.visibility.1')
            click('info.transferHistory'); click('info.pageVisibility')
            current=click('info.visibility.2')
            assert current['selected']==0 and current['visible_mask']==11, current
            checks.append({'case':'hide-current-and-restore','state':current})
            for i in [0,1]:
                click("info.pageVisibility"); click("info.visibility."+str(i))
            click("info.pageVisibility")
            current=click("info.visibility.3")
            assert current["visible_mask"]==8 and current["selected"]==3, current
            checks.append({"case":"cannot-hide-last-page","state":current})
            click("info.visibility.1")
            click("info.pageVisibility"); click("info.visibility.0")
            click('info.playHistory'); capture('hidden-transfer')
            for theme in [0,1,2,3,10]:
                command(cmd='set_theme',theme=theme)
                capture('theme-'+str(theme))
            command(cmd='set_theme',theme=0)
            command(cmd="info_page_layout",floating=True,width=320)
            current=click("info.metadataSummary")
            assert current["floating"] and current["selected"]==0, current
            checks.append({"case":"floating-click-does-not-drag","state":current})
            capture("floating", "info.metadataSummary")
            command(cmd="info_page_layout",floating=False,width=320)
            settled()
            command(cmd="info_page_layout",floating=True,width=220)
            current=settled()
            assert "info.playHistory" not in current["zones"], current
            click("info.pageVisibility"); current=click("info.select.3")
            assert current["selected"]==3, current
            capture("narrow-floating", "info.playHistory")
            checks.append({"case":"narrow-menu-switch","state":current})
            command(cmd="info_page_layout",floating=False,width=320)
            click("info.playHistory")
        (output/'report.json').write_text(json.dumps({'identity':identity,
            'exe_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(), 'checks':checks,
            'limitation':'App-local ImGui input; no physical mouse or human acceptance.'},ensure_ascii=False,indent=2),encoding='utf-8')
        print('PASS: real Info page UI checks')
    except Exception:
        if hwnd:
            try: ImageGrab.grab(window=hwnd).save(output / "failure.png")
            except Exception: pass
        raise
    finally:
        if hwnd: user32.PostMessageW(hwnd,0x0010,0,0)
        try: process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.terminate(); process.wait(timeout=5)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',type=Path,required=True)
    parser.add_argument('--isolation-root',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--expect-saved',action='store_true')
    parser.add_argument('--prepare-demo',action='store_true')
    parser.add_argument('--edit-suite',action='store_true')
    parser.add_argument('--edit-tray-suite',action='store_true')
    parser.add_argument('--edit-update-suite',action='store_true')
    parser.add_argument('--edit-new-db-suite',action='store_true')
    run(parser.parse_args())
