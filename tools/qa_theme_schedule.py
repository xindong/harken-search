"""Verify day/night palettes in the actual isolated EXE on a private desktop.

Requires pywin32 and Pillow. Pointer events enter only the app's ImGui queue.
The caller supplies a disposable profile and a real library snapshot inside it.
Clock and draft-color seams are compiled only into isolated diagnostic packages.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import time
import traceback
from qa_required_library_setup import PrivateDesktop, capture, wait_state
from qa_transport_themes import Driver

def run(args):
    import win32con, win32event, win32process, win32gui
    exe, root, out = args.exe.resolve(), args.isolation_root.resolve(), args.output.resolve()
    library = args.library.resolve()
    assert library.is_relative_to(root) and library.is_file(), 'Use a library snapshot inside the isolated profile'
    out.mkdir(parents=True, exist_ok=False)
    runtime = root/'profile/runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    driver, desktop = Driver(runtime, out), PrivateDesktop()
    process = None
    client_origin = (0,0)
    report = dict(exe=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  started_at=datetime.now(timezone.utc).isoformat(), checks=[], exit_codes=[])
    profiles = root/'profile/Local/HarkenSearch'
    day_file, night_file = profiles/'theme_schedule_day.json', profiles/'theme_schedule_night.json'
    def command(**payload): return driver.command(payload)
    def check(name, **details):
        report['checks'].append(dict(case=name, **details))
        print(name, flush=True)
    def state(): return command(cmd='theme_schedule')
    def wait(predicate, label):
        deadline = time.monotonic()+12
        while time.monotonic() < deadline:
            value = state()
            if predicate(value): return value
            time.sleep(.08)
        raise AssertionError((label, value))
    def settle():
        first=state()['frame']
        return wait(lambda s:s['frame']>first+3, 'rendered frames')
    def start(minute):
        nonlocal process, client_origin
        previous=os.environ.get('HARKEN_THEME_QA_MINUTE')
        os.environ['HARKEN_THEME_QA_MINUTE']=str(minute)
        try: process,pid=desktop.start(exe)
        finally:
            if previous is None: os.environ.pop('HARKEN_THEME_QA_MINUTE',None)
            else: os.environ['HARKEN_THEME_QA_MINUTE']=previous
        identity=driver.command({'cmd':'magic_eq_identity'},timeout=60)
        assert identity['pid']==pid and Path(identity['exe_path']).resolve()==exe,identity
        assert Path(identity['isolation_root']).resolve()==root,identity
        wait_state(driver,lambda s:s['main_ui_ready'],'startup',60)
        note=command(cmd='qa_narration',action='state')
        assert note['open'] and not note['error'],note
        command(cmd='qa_narration',action='close'); settle()
        current=command(cmd='get_state')
        if current['db_path']!=str(library):
            command(cmd='switch_db',path=str(library))
        current=wait_state(driver,lambda s:s['total_count']>0 and not s['db_switch_in_progress'],'real library',60)
        report['library']=dict(path=current['db_path'],total=current['total_count'])
        command(cmd='dock_pointer',phase='focus'); settle()
        client_origin = win32gui.ClientToScreen(identity['hwnd'],(0,0))
        report['client_origin'] = client_origin
        return state()
    def stop():
        nonlocal process
        if process is None: return
        if win32event.WaitForSingleObject(process,0)==win32con.WAIT_TIMEOUT:
            command(cmd='info_edit_input',action='exit')
        assert win32event.WaitForSingleObject(process,15000)==win32con.WAIT_OBJECT_0,'exit timeout'
        code=win32process.GetExitCodeProcess(process)
        report['exit_codes'].append(code)
        process.Close(); process=None
        assert code==0,code
    def audit():
        first=command(cmd='ordinary_button_audit')
        deadline=time.monotonic()+8
        while time.monotonic()<deadline:
            snapshot=command(cmd='ordinary_button_audit')
            if snapshot['frame']>first['current_frame']+2 and snapshot['current_frame']-snapshot['frame']<=2:
                return snapshot['checks']
            time.sleep(.08)
        raise AssertionError('No fresh UI button measurements')
    def find(label):
        buttons=[b for b in audit() if b['label']==label and not b['disabled']]
        assert len(buttons)==1,(label,buttons)
        button=buttons[0]
        x0,y0,x1,y1=button['bounds']; l,t,r,b=button['clip']
        assert x0>=l-1 and y0>=t-1 and x1<=r+1 and y1<=b+1,button
        return button
    def click_at(x,y):
        # Button observations use screen coordinates; the app-owned pointer
        # accepts client coordinates and adds its main viewport origin itself.
        x,y=x-client_origin[0],y-client_origin[1]
        for phase in ('hover','down','up'):
            command(cmd='theme_schedule',action='pointer',phase=phase,x=x,y=y)
            settle()
    def click(label):
        button=find(label)
        a,b,c,d=button['bounds']
        click_at((a+c)/2,(b+d)/2)
    def settings():
        command(cmd='ordinary_button_window',module='settings',action='open'); settle()
        # First navigation item is aligned with the search row; derive coordinates
        # from current measured controls instead of using screen-size assumptions.
        clear,save=find('清除'),find('保存并关闭')
        click_at(save['bounds'][0]+20,(clear['bounds'][1]+clear['bounds'][3])/2)
        find('编辑白天配色'); find('编辑夜间配色')
    def close_settings(): click('保存并关闭')
    def clock(minute):
        command(cmd='theme_schedule',action='clock',minute=minute)
        period='day' if 480<=minute<960 else 'night'
        value=wait(lambda s:s['period']==period and s['theme']==7 and
            s['active_bg']==s[period+'_bg'] and s['active_accent']==s[period+'_accent'] and
            s['imgui_bg']==s['active_bg'], 'scheduled colors rendered')
        settle()
        return value
    def draft(key,value): command(cmd='theme_schedule',action='draft_color',key=key,rgba=value); settle()
    def snapshot(path): return (path.read_bytes(),path.stat().st_mtime_ns) if path.exists() else None
    def open_editor(day):
        click('编辑白天配色' if day else '编辑夜间配色')
        return wait(lambda s:s['editor_open'] and s['editor_period']==(0 if day else 1),'editor opens')
    try:
        initial=start(480)
        assert initial['period']=='day' and initial['active_bg']==initial['day_bg'],initial
        check('day-startup-real-library',state=initial)
        settings(); capture(driver,out/'settings-day.png')
        night_before=snapshot(night_file)
        open_editor(True); capture(driver,out/'editor-day.png')
        draft('toolbarAccent',[.2,.42,.64,1])
        click('保存配色')
        wait(lambda s:not s['editor_open'],'day saved')
        assert snapshot(night_file)==night_before,'day save touched night'
        day_saved=snapshot(day_file)
        assert day_saved and len(json.loads(day_saved[0])['colors'])==113
        check('day-save-is-independent',state=state())
        open_editor(True)
        draft('toolbarAccent',[.9,.1,.2,1]); click('取消修改')
        wait(lambda s:not s['editor_open'],'cancel closes')
        assert snapshot(day_file)==day_saved,'cancel changed day file'
        open_editor(True)
        draft('mainWindowBG',[.1,.1,.1,1]); click('保存配色')
        rejected=state()
        assert rejected['editor_open'] and rejected['editor_status'],rejected
        assert snapshot(day_file)==day_saved,'invalid palette changed file'
        capture(driver,out/'light-palette-guard.png')
        click('取消修改'); wait(lambda s:not s['editor_open'],'invalid draft cancelled')
        check('cancel-and-day-category-guard',state=rejected)
        open_editor(False)
        draft('toolbarAccent',[.65,.42,.86,1]); capture(driver,out/'editor-night.png')
        # A clock jump must not overwrite a live draft.
        command(cmd='theme_schedule',action='clock',minute=960); settle()
        assert state()['editor_open'] and state()['active_accent']!='ff6f98c1'
        command(cmd='theme_schedule',action='clock',minute=480); settle()
        click('保存配色'); wait(lambda s:not s['editor_open'],'night saved')
        assert snapshot(day_file)==day_saved,'night save touched day'
        night_saved=snapshot(night_file)
        assert night_saved
        check('night-save-is-independent-and-preview-survives-clock-jump',state=state())
        for minute in (479,480,959,960,0,1439,480):
            value=clock(minute)
            capture(driver,out/f'clock-{minute//60:02d}-{minute%60:02d}.png')
            check('clock-boundary-'+str(minute),state=value)
        for theme in (0,1,2,3,10):
            command(cmd='set_theme',theme=theme)
            value=clock(480)
            assert snapshot(day_file)==day_saved and snapshot(night_file)==night_saved
            check('manual-theme-restored-'+str(theme),state=value)
        close_settings(); capture(driver,out/'library-day.png')
        stop()
        value=start(960)
        assert value['period']=='night' and value['active_accent']==value['night_accent'],value
        assert snapshot(day_file)==day_saved and snapshot(night_file)==night_saved
        check('night-restart-preserves-both',state=value)
        settings(); capture(driver,out/'settings-night.png'); close_settings()
        capture(driver,out/'library-night.png'); stop()
        # Corrupt one slot while stopped, then ensure only that slot falls back.
        day_file.write_text('{broken',encoding='utf-8')
        value=start(480)
        assert value['day_warning'] and not value['night_warning'],value
        assert snapshot(night_file)==night_saved,'damaged day affected night'
        settings(); capture(driver,out/'damaged-day-fallback.png'); close_settings()
        check('corrupt-day-falls-back-independently',state=value)
        stop()
        day_file.write_bytes(day_saved[0])
        value=start(480)
        assert not value['day_warning'] and value['active_accent']==value['day_accent'],value
        check('day-restart-preserves-edited-palette',state=value)
        # Leave the delivered profile on the real local clock.
        command(cmd='theme_schedule',action='clock',minute=-1); settle()
        stop()
        report['passed']=True
    except Exception:
        report['passed']=False; report['error']=traceback.format_exc()
        try: capture(driver,out/'failure.png')
        except Exception: pass
        raise
    finally:
        try: stop()
        finally:
            desktop.close()
            report['finished_at']=datetime.now(timezone.utc).isoformat()
            report['not_covered']=['User visual acceptance', 'Physical sleep/resume and OS timezone changes; equivalent wall-clock jumps tested', 'Every individual color-picker gesture; draft-color seam used with real open/save/cancel buttons']
            (out/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',required=True,type=Path)
    parser.add_argument('--isolation-root',required=True,type=Path)
    parser.add_argument('--library',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    run(parser.parse_args())
