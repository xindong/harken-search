"""Verify portrait Dock resizing and rendered cover size in an isolated real package.

Uses application-owned input only; never moves the shared desktop cursor.
"""
from __future__ import annotations
import argparse, hashlib, json, os, re, subprocess, time, uuid
from pathlib import Path
from PIL import Image, ImageGrab
from qa_transport_themes import Driver, THEMES, window_api


def run(args):
    exe, root, output = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    runtime=root/'profile/runtime'
    driver=Driver(runtime,output); windows=window_api()
    process=None; hwnd=None; cases=[]; identity={}
    def command(**payload): return driver.command(payload)
    def settled(predicate=lambda state: True):
        first=command(cmd='dock_state'); deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            state=command(cmd='dock_state')
            if state['frame']>=first['frame']+5 and predicate(state): return state
            time.sleep(.06)
        raise AssertionError(state)
    def launch(width=1100,height=1600):
        nonlocal process,hwnd,identity
        startup=subprocess.STARTUPINFO(); startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow=4
        process=subprocess.Popen([str(exe)],cwd=exe.parent,startupinfo=startup,
            env=dict(os.environ,AURA_HARNESS_MULTI_INSTANCE='1',AURA_HARNESS_LANE_KIND='functional',AURA_DISABLE_SINGLE_INSTANCE='1'))
        identity=driver.command({'cmd':'magic_eq_identity'},timeout=60)
        assert identity['pid']==process.pid and Path(identity['exe_path']).resolve()==exe,identity
        assert Path(identity['isolation_root']).resolve()==root,identity
        hwnd=identity['hwnd']
        windows.ShowWindow(hwnd,4)
        deadline=time.monotonic()+45
        while not command(cmd='magic_eq_state')['main_ui_ready']:
            assert time.monotonic()<deadline,'Main UI not ready'
            time.sleep(.1)
        windows.SetWindowPos(hwnd,None,40,40,width,height,0x0014)
        if (exe.parent/'qa-narration.json').is_file(): command(cmd='qa_narration',action='close')
        command(cmd='dock_pointer',phase='focus'); settled()
    def artwork():
        command(cmd='info_page_state'); settled()
        for phase in ('hover','down','up'):
            command(cmd='info_page_pointer',key='info.artwork',phase=phase); settled()
    def covers(size):
        command(cmd='set_cover_wall_thumb_size',size=size)
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            settled(); wall=command(cmd='get_state')['cover_wall']
            if wall['grid_columns']>0 and wall['visible_ready']>0 and not wall['loading']:
                return wall
        raise AssertionError(wall)
    def capture(name,wall=None):
        state=settled()
        if args.framebuffer:
            key=uuid.uuid4().hex; command(cmd='qa_capture',capture_key=key)
            raw=runtime/(key+'.ppm'); deadline=time.monotonic()+8
            while True:
                try:
                    shot=Image.open(raw).convert('RGB')
                    if len(shot.getcolors(shot.width*shot.height) or [])>20: break
                except OSError: pass
                assert time.monotonic()<deadline,'No complete framebuffer'
                time.sleep(.08)
        else: shot=ImageGrab.grab(window=hwnd).convert('RGB')
        assert len(shot.getcolors(shot.width*shot.height) or [])>20,'Blank capture'
        path=output/(name+'.png'); shot.save(path)
        cases.append(dict(name=name,pid=process.pid,state=state,cover_wall=wall,screenshot=str(path),
            screenshot_sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
        print('PASS:',name,flush=True)
        return state
    def dock(side):
        command(cmd='info_page_layout',floating=True,width=480)
        state=settled(lambda s:s['panels']['info']['floating']); results=state['panels']['search-results']
        y=results['y']+10 if side==2 else results['y']+results['h']-10
        panel=state['panels']['info']
        # A remembered floating window can cover the destination midpoint.
        # Drop outside its original rectangle to express an actual edge move.
        candidates=[results['x']+results['w']*fraction for fraction in (.5,.15,.85)]
        x=next((candidate for candidate in candidates if not (
            panel['x']<=candidate<panel['x']+panel['w'] and
            panel['y']<=y<panel['y']+panel['h'])),candidates[0])
        for phase in ('hover','down'):
            pointer=command(cmd='dock_pointer',phase=phase,module='info',part='title'); settled()
        # Always cross the drag threshold, even when the remembered title is
        # already directly over the destination edge.
        command(cmd='dock_pointer',phase='move',x=pointer['x']+60,y=pointer['y']+60)
        settled(lambda s:s['dragging'])
        command(cmd='dock_pointer',phase='move',x=x,y=y)
        state=settled(lambda s:s['dragging'])
        assert state['preview_side']==side and state['preview_kind']==1,state
        command(cmd='dock_pointer',phase='up',x=x,y=y)
        return settled(lambda s:not s['dragging'] and not s['panels']['info']['floating'])
    def resize(height,module='info'):
        state=settled(); panel=state['panels'][module]; top=state['layout'][2]==module
        x=panel['x']+panel['w']*.5
        start=panel['y']+panel['h']+3 if top else panel['y']-3
        y=start+(height-panel['h'])*(1 if top else -1)
        for phase in ('hover','down'):
            command(cmd='dock_pointer',phase=phase,module=module,part='band-splitter'); settled()
        command(cmd='dock_pointer',phase='move',x=x,y=y); settled()
        command(cmd='dock_pointer',phase='up',x=x,y=y)
        return settled()
    def column_width(module,width):
        state=settled(); panel=state['panels'][module]
        left=state['layout'][0]==module
        for phase in ('hover','down'):
            point=command(cmd='dock_pointer',phase=phase,module=module,part='column-splitter'); settled()
        x=point['x']+(width-panel['w'])*(1 if left else -1)
        command(cmd='dock_pointer',phase='move',x=x,y=point['y']); settled()
        command(cmd='dock_pointer',phase='up',x=x,y=point['y'])
        state=settled()
        assert abs(state['panels'][module]['w']-width)<=2,state
        return state
    try:
        launch(); command(cmd='set_theme',theme=2)
        column_width('filter',250)
        for side,label in ((2,'top'),(3,'bottom')):
            state=dock(side); artwork(); wall=covers(56)
            assert wall['grid_columns']>8 and 56<=wall['rendered_thumb_size']<63,wall
            resize(220); capture('portrait-'+label+'-compact',wall)
            state=resize(680)
            assert abs(state['panels']['info']['h']-680)<=2,state
            assert state['panels']['search-results']['h']>=199,state
            capture('portrait-'+label+'-expanded',covers(56))
            enlarged=covers(132)
            assert enlarged['grid_columns']<wall['grid_columns'] and enlarged['rendered_thumb_size']>100,enlarged
            capture('portrait-'+label+'-large-covers',enlarged)
            state=resize(3000)
            assert state['panels']['search-results']['h']>=199,state
            assert state['panels']['info']['h']>680,state
            capture('portrait-'+label+'-maximum',covers(56))
            state=resize(180)
            assert 179<=state['panels']['info']['h']<=182,state
            capture('portrait-'+label+'-minimum',covers(56))
            resize(680)
            windows.SetWindowPos(hwnd,None,40,40,1100,900,0x0014)
            small=settled()
            assert not small['panels']['info']['floating'] and small['panels']['search-results']['h']>=199,small
            capture('landscape-'+label+'-fitted',covers(56))
            # Resizing a temporarily reduced band starts from its visible height.
            before=small['panels']['info']['h']; state=resize(before-30)
            assert abs(state['panels']['info']['h']-(before-30))<=2,(small,state)
            capture('landscape-'+label+'-drag-after-shrink',covers(56))
            windows.SetWindowPos(hwnd,None,40,40,1100,1600,0x0014); settled(); resize(680)
        saved=settled()
        command(cmd='info_edit_input',action='exit'); process.wait(timeout=20); hwnd=None
        launch(); restarted=settled()
        assert restarted['layout']==saved['layout'],(saved,restarted)
        assert abs(restarted['panels']['info']['h']-680)<=2,restarted
        capture('portrait-restart-restores-height',covers(56))
        redocked=dock(2)
        assert abs(redocked['panels']['info']['h']-680)<=2,redocked
        capture('portrait-redock-restores-height',covers(56))
        resize(500)
        command(cmd='similar_index',action='open',floating=True,width=640,height=360)
        state=settled(lambda s:s['panels'].get('similar-sounds',{}).get('floating',False))
        results=state['panels']['search-results']
        x,y=results['x']+results['w']*.5,results['y']+results['h']-10
        for phase in ('hover','down'):
            pointer=command(cmd='dock_pointer',phase=phase,module='similar-sounds',part='title'); settled()
        command(cmd='dock_pointer',phase='move',x=pointer['x']+60,y=pointer['y']+60)
        settled(lambda s:s['dragging'])
        command(cmd='dock_pointer',phase='move',x=x,y=y)
        state=settled(lambda s:s['dragging'])
        assert state['preview_side']==3 and state['preview_kind']==1,state
        command(cmd='dock_pointer',phase='up',x=x,y=y)
        settled(lambda s:not s['dragging'])
        state=resize(420,'similar-sounds')
        assert state['layout'][2:]==['info','similar-sounds'],state
        assert abs(state['panels']['info']['h']-500)<=2,state
        assert abs(state['panels']['similar-sounds']['h']-420)<=2,state
        assert state['panels']['search-results']['h']>=199,state
        capture('portrait-both-tall-bands',covers(56))
        command(cmd='similar_index',action='close'); settled(); resize(260)
        for theme,_ in THEMES:
            command(cmd='set_theme',theme=theme); settled()
            wall=covers(56)
            assert wall['grid_columns']>8 and wall['rendered_thumb_size']<63,wall
            capture('portrait-theme-'+str(theme),wall)
        command(cmd='set_theme',theme=2); settled()
        capture('portrait-compact-preview',covers(56))
        # Load the user's swapped layout as a saved-settings fixture, then use
        # actual splitter input to resize both columns and verify persistence.
        command(cmd='info_edit_input',action='exit'); process.wait(timeout=20); hwnd=None
        settings=root/'profile/Local/HarkenSearch/settings.ini'
        portrait_settings=settings.read_text('utf-8')
        (output/'portrait-settings.ini').write_text(portrait_settings,encoding='utf-8')
        swapped=portrait_settings
        for key,value in dict(panelSidebar_dockSide=1,sidebarDockSide=1,panelSidebar_floating=0,
                              panelFileInfo_dockSide=0,fileInfoDockSide=0,panelFileInfo_floating=0,
                              panelSidebar_dockedWidth=400,panelFileInfo_dockedWidth=400).items():
            swapped,count=re.subn('^'+key+'=.*$',key+'='+str(value),swapped,flags=re.M)
            assert count==1,(key,count)
        settings.write_text(swapped,encoding='utf-8')
        launch(1800,1000)
        assert settled()['layout']==['info','filter',None,None]
        for theme,_ in THEMES:
            command(cmd='set_theme',theme=theme); settled()
            capture('swapped-columns-theme-'+str(theme),covers(56))
        command(cmd='set_theme',theme=2); settled()
        state=settled(); column_width('info',state['panels']['info']['w']+60)
        capture('swapped-left-resized',covers(56))
        state=settled(); column_width('filter',state['panels']['filter']['w']+40)
        saved=capture('swapped-right-resized',covers(56))
        command(cmd='info_edit_input',action='exit'); process.wait(timeout=20); hwnd=None
        launch(1800,1000); restarted=settled()
        assert restarted['layout']==saved['layout'],(saved,restarted)
        for module in ('info','filter'):
            assert abs(restarted['panels'][module]['w']-saved['panels'][module]['w'])<=2,(saved,restarted)
        capture('swapped-restart-restores-widths',covers(56))
        command(cmd='info_edit_input',action='exit'); process.wait(timeout=20); hwnd=None
        settings.write_text(portrait_settings,encoding='utf-8')
        launch(); capture('ready-for-user',covers(56))
        report=dict(passed=True,identity=identity,exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
            cases=cases,input_method='app-owned input; no desktop cursor',
            capture_method='OpenGL framebuffer' if args.framebuffer else 'native window')
        (output/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    except Exception as error:
        (output/'failure.json').write_text(json.dumps(dict(identity=identity,cases=cases,error=str(error)),ensure_ascii=False,indent=2),encoding='utf-8')
        raise
    finally:
        if process and process.poll() is None:
            try: command(cmd='info_edit_input',action='exit'); process.wait(timeout=20)
            except (RuntimeError,TimeoutError,subprocess.TimeoutExpired):
                process.terminate(); process.wait(timeout=5)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',type=Path,required=True)
    parser.add_argument('--isolation-root',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--framebuffer',action='store_true',help='Use the local-only QA framebuffer patch')
    run(parser.parse_args())
