"""Verify the actual Info shortcut row, visibility menu and restart persistence.

Uses a disposable compile-time isolated package and the local QA framebuffer
supplement. Never moves the shared desktop pointer.
"""
import argparse, hashlib, json, os, re, subprocess, time, uuid
import ctypes
import ctypes.wintypes as w
from pathlib import Path
from PIL import Image, ImageGrab
from qa_transport_themes import Driver, THEMES, window_api


def run(args):
    exe,root,out=args.exe.resolve(),args.isolation_root.resolve(),args.output.resolve()
    out.mkdir(parents=True,exist_ok=False)
    runtime=root/'profile/runtime'; driver=Driver(runtime,out); windows=window_api()
    settings=root/'profile/Local/HarkenSearch/settings.ini'
    original=settings.read_text('utf-8-sig'); (out/'settings-before.ini').write_text(original,encoding='utf-8')
    process=None; identity={}; cases=[]; failure=None
    def command(**kw):return driver.command(kw)
    def settled():
        first=command(cmd='info_page_state'); end=time.monotonic()+20
        while time.monotonic()<end:
            state=command(cmd='info_page_state')
            if state['frame']>=first['frame']+6 and state['zones']:return state
            time.sleep(.05)
        raise AssertionError(state)
    def click(key):
        assert key in settled()['zones'],key
        for phase in ('hover','down','up'):
            command(cmd='info_page_pointer',key=key,phase=phase);settled()
        return settled()
    def close_menu():
        command(cmd='info_edit_input',action='escape_down');settled()
        command(cmd='info_edit_input',action='escape_up');settled()
    def menu():
        close_menu();click('info.pageVisibility')
        assert 'info.visibility.similarSounds' in settled()['zones']
    def aligned(visible=True):
        state=settled(); zones=state['zones']; shortcut=zones.get('info.similarSounds')
        assert bool(shortcut)==visible,('Similar Sounds is not part of the expected title row',state)
        ids=['info.metadataSummary','info.artwork','info.transferHistory','info.playHistory','info.pageVisibility']
        buttons=[zones[key] for key in ids if key in zones]
        assert buttons
        if shortcut:
            assert all(shortcut['x']<button['x'] for button in buttons),state
            buttons.insert(0,shortcut)
        assert all(abs(button['y']-buttons[0]['y'])<.1 and abs(button['h']-buttons[0]['h'])<.1 and button['w']==24 for button in buttons),state
        for a,b in zip(buttons,buttons[1:]):assert abs(b['x']-a['x']-24)<.1,state
        return state
    def ready_covers():
        end=time.monotonic()+30
        while time.monotonic()<end:
            settled();wall=command(cmd='get_state')['cover_wall']
            if (wall['display_count']==48 and wall['visible_tiles']>0
                    and wall['visible_ready']==wall['visible_tiles']
                    and wall['pending_cover_work']==0 and not wall['loading']):return
        raise AssertionError(wall)
    def capture(name,visible=True,native_key=None):
        state=aligned(visible)
        case=dict(name=name,state=state,pid=process.pid)
        if native_key:
            # Menus and floating panels may have their own native viewport.
            # Capture only this process's smallest window containing the item.
            zone=state['zones'][native_key]
            x,y=zone['x']+zone['w']/2,zone['y']+zone['h']/2
            candidates=[]
            windows.GetWindowRect.argtypes=[w.HWND,ctypes.POINTER(w.RECT)]
            windows.IsWindowVisible.argtypes=[w.HWND]
            callback=ctypes.WINFUNCTYPE(w.BOOL,w.HWND,w.LPARAM)
            @callback
            def visit(window,_):
                pid=w.DWORD();rect=w.RECT()
                windows.GetWindowThreadProcessId(window,ctypes.byref(pid))
                if pid.value==process.pid and windows.IsWindowVisible(window):
                    windows.GetWindowRect(window,ctypes.byref(rect))
                    if rect.left<=x<rect.right and rect.top<=y<rect.bottom:
                        candidates.append(((rect.right-rect.left)*(rect.bottom-rect.top),window))
                return True
            windows.EnumWindows(visit,0)
            assert candidates,(native_key,state)
            window=min(candidates)[1]
            shot=ImageGrab.grab(window=window).convert('RGB')
            assert len(shot.getcolors(shot.width*shot.height) or [])>20
            case.update(capture_method='read-only native viewport capture',hwnd=window)
        else:
            point=w.POINT(0,0)
            assert windows.ClientToScreen(identity['hwnd'],ctypes.byref(point))
            case['client_origin']=[point.x,point.y]
            key=uuid.uuid4().hex;command(cmd='qa_capture',capture_key=key);raw=runtime/(key+'.ppm');end=time.monotonic()+10
            while True:
                try:shot=Image.open(raw).convert('RGB');shot.load();break
                except OSError:
                    # Capture output can be missing or incomplete until the
                    # application finishes writing; keep the bounded retry.
                    pass
                assert time.monotonic()<end;time.sleep(.05)
            case['capture_method']='main-window OpenGL framebuffer'
        path=out/(name+'.png');shot.save(path)
        case.update(screenshot=str(path),screenshot_sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        cases.append(case);print('PASS:',name,flush=True)
        return state
    def launch():
        nonlocal process,identity
        startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=4
        process=subprocess.Popen([str(exe)],cwd=exe.parent,startupinfo=startup,
            env=dict(os.environ,AURA_HARNESS_MULTI_INSTANCE='1',AURA_HARNESS_LANE_KIND='functional',AURA_DISABLE_SINGLE_INSTANCE='1'))
        identity=driver.command({'cmd':'magic_eq_identity'},timeout=60)
        assert identity['pid']==process.pid and Path(identity['exe_path']).resolve()==exe,identity
        assert Path(identity['isolation_root']).resolve()==root,identity
        windows.ShowWindow(identity['hwnd'],4);end=time.monotonic()+45
        while not command(cmd='magic_eq_state')['main_ui_ready']:
            assert time.monotonic()<end;time.sleep(.1)
        windows.SetWindowPos(identity['hwnd'],None,40,40,1400,1000,0x0014)
        command(cmd='qa_narration',action='close');command(cmd='dock_pointer',phase='focus');settled()
        command(cmd='set_cover_wall_thumb_size',size=56);ready_covers()
    def stop():
        command(cmd='info_edit_input',action='exit');process.wait(timeout=20)
    def configure(**values):
        text=settings.read_text('utf-8-sig')
        for key,value in values.items():
            text,n=re.subn('^'+key+'=.*$',key+'='+str(value),text,flags=re.M)
            if not n:text+='\n'+key+'='+str(value)
        settings.write_text(text,encoding='utf-8')
    try:
        # Legacy profile with no shortcut preference must keep the entry visible.
        settings.write_text(re.sub(r'^infoShowSimilarSounds=.*\n?','',original,flags=re.M),encoding='utf-8')
        configure(panelFileInfo_floating=0,panelFileInfo_dockSide=1,fileInfoDockSide=1,
            panelFileInfo_dockedWidth=400,panelSidebar_dockSide=0,sidebarDockSide=0,
            panelSidebar_dockedWidth=250,panelSimilarSounds_visible=0,infoSelectedPage=1,infoPageVisibleMask=15)
        launch();initial=capture('default-leftmost-aligned')
        selected,mask=initial['selected'],initial['visible_mask']
        click('info.similarSounds');dock=command(cmd='dock_state')
        assert 'similar-sounds' in dock['panels'],dock
        assert settled()['selected']==selected
        capture('opens-similar-without-changing-page')
        command(cmd='similar_index',action='close');settled()
        menu();capture('visibility-menu',native_key='info.visibility.similarSounds')
        current=click('info.visibility.similarSounds')
        assert current['selected']==selected and current['visible_mask']==mask,current
        capture('hidden-shortcut',False)
        stop();assert re.search(r'^infoShowSimilarSounds=0$',settings.read_text('utf-8'),re.M)
        launch();capture('hidden-after-restart',False)
        menu();click('info.visibility.similarSounds');capture('restored-shortcut')
        stop();assert re.search(r'^infoShowSimilarSounds=1$',settings.read_text('utf-8'),re.M)
        launch();capture('restored-after-restart')
        for theme,_ in THEMES:
            command(cmd='set_theme',theme=theme);ready_covers();capture('theme-'+str(theme))
        command(cmd='set_theme',theme=2)
        # A shortcut is not a content page: it must not defeat the final-page guard.
        for i in (0,2,3):menu();click('info.visibility.'+str(i))
        assert settled()['visible_mask']==2
        menu();click('info.visibility.1');assert settled()['visible_mask']==2
        click('info.visibility.similarSounds');capture('last-page-shortcut-hidden',False)
        menu();click('info.visibility.similarSounds');capture('last-page-shortcut-restored')
        for i in (0,2,3):menu();click('info.visibility.'+str(i))
        command(cmd='info_page_layout',floating=False,width=180);settled()
        capture('narrow-docked')
        assert len([key for key in settled()['zones'] if key in ('info.metadataSummary','info.artwork','info.transferHistory','info.playHistory')])==1
        command(cmd='info_page_layout',floating=True,width=220);settled()
        floating=capture('narrow-floating',native_key='info.similarSounds')
        window=next(w for w in floating['layout_windows'] if w['name']=='Info##FloatingFileInfo')
        # Floating Info clamps to 240px: all actions fit at that supported minimum.
        assert window['w']==240,window
        assert floating['zones']['info.similarSounds']['x']>=window['x']+40,floating
        more,close=floating['zones']['info.pageVisibility'],floating['zones']['info.close']
        assert more['x']+more['w']<=close['x'],floating
        click('info.similarSounds')
        after=settled()
        assert after['selected']==selected
        assert next(w for w in after['layout_windows'] if w['name']=='Info##FloatingFileInfo')==window
        command(cmd='similar_index',action='close');settled()
        menu();click('info.visibility.similarSounds');capture('narrow-floating-hidden',False,native_key='info.pageVisibility')
        menu();click('info.visibility.similarSounds');capture('narrow-floating-restored',native_key='info.similarSounds')
        command(cmd='info_page_layout',floating=False,width=400);settled();stop()
        for side,label in ((0,'left'),(2,'top'),(3,'bottom')):
            configure(panelFileInfo_dockSide=side,fileInfoDockSide=side,panelFileInfo_floating=0,
                panelSidebar_dockSide=1 if side==0 else 0,sidebarDockSide=1 if side==0 else 0,panelFileInfo_dockedHeight=300)
            launch();capture('dock-'+label);stop()
        configure(panelFileInfo_dockSide=0,fileInfoDockSide=0,panelSidebar_dockSide=1,sidebarDockSide=1)
        launch();menu();click('info.visibility.2');capture('ready-visible')
        menu();capture('ready-menu',native_key='info.visibility.similarSounds')
        click('info.visibility.similarSounds');capture('ready-hidden',False)
        menu();click('info.visibility.similarSounds');capture('ready-for-user');stop()
    except Exception as error:
        failure=f'{type(error).__name__}: {error}';raise
    finally:
        if process and process.poll() is None:
            try:stop()
            except (RuntimeError,TimeoutError,subprocess.TimeoutExpired):process.terminate();process.wait(timeout=5)
        if failure:settings.write_text(original,encoding='utf-8')
        (out/'report.json').write_text(json.dumps(dict(passed=failure is None,error=failure,cases=cases,identity=identity,
            exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),input_method='app-owned; no desktop pointer',
            capture_method='actual OpenGL framebuffer and read-only native viewport captures'),ensure_ascii=False,indent=2),encoding='utf-8')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--exe',type=Path,required=True);p.add_argument('--isolation-root',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    run(p.parse_args())
