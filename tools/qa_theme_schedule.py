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
        if args.qa_narration:
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
        clear,save=find('清除'),find('保存并关闭')
        click_at(save['bounds'][0]+20,(clear['bounds'][1]+clear['bounds'][3])/2)
        settle()
    def close_settings(): click('保存并关闭')
    def visible(rect): return len(rect)==4 and rect[2]>rect[0] and rect[3]>rect[1]
    def click_rect(rect):
        assert visible(rect),rect
        a,b,c,d=rect
        click_at((a+c)/2,(b+d)/2)
    def automatic(enabled):
        if (state()['mode']==1) != enabled:
            click_rect(state()['auto_toggle_bounds'])
        return wait(lambda s:(s['mode']==1)==enabled,'automatic checkbox')
    def assert_groups(enabled):
        value=settle(); ui=value['ui']
        assert len(ui['stock'])==(0 if enabled else 5),('ordinary themes must show immediately when unchecked',ui)
        selectors=[r for r in ui.get('selector_buttons',[]) if visible(r)]
        assert len(selectors)==(2 if enabled else 0),('day/night selectors must be exclusive with ordinary themes',ui)
        assert not any(visible(r) for r in ui.get('cards',[])),'large day/night cards remain'
        assert not visible(ui.get('other_themes',[])),'ordinary themes must not require expansion'
        assert ui.get('auto_checkbox') is True,'the automatic control must be a checkbox'
        l,t,r,b=ui['clip']
        for bounds in [value['auto_toggle_bounds'],*selectors]:
            assert bounds[0]>=l-1 and bounds[1]>=t-1 and bounds[2]<=r+1 and bounds[3]<=b+1,('clipped control',bounds,ui['clip'])
        if selectors:
            assert selectors[0][1]==selectors[1][1] and selectors[0][2]<selectors[1][0],ui
            assert all(rect[3]-rect[1]<=48 for rect in selectors),'selectors should be compact'
        return value
    def click_stock(theme):
        item=next(x for x in settle()['ui']['stock'] if x['theme']==theme)
        click_rect(item['bounds']); settle()
    def clock(minute):
        command(cmd='theme_schedule',action='clock',minute=minute)
        period='day' if 480<=minute<960 else 'night'
        value=wait(lambda s:s['period']==period and s['mode']==1 and s['theme']==7 and
            s['active_bg']==s[period+'_bg'] and s['active_accent']==s[period+'_accent'] and
            s['imgui_bg']==s['active_bg'],'scheduled colors rendered')
        settle()
        return value
    def draft(key,value): command(cmd='theme_schedule',action='draft_color',key=key,rgba=value); settle()
    def snapshot(path): return (path.read_bytes(),path.stat().st_mtime_ns) if path.exists() else None
    def open_selector(day):
        index=0 if day else 1
        click_rect(settle()['ui']['selector_buttons'][index])
        value=settle()
        options=[o for o in value['ui']['preset_options'] if o['period']==index]
        expected={0,1,2,3,10}
        assert {o['theme'] for o in options}==expected,('both periods must offer all five themes',options)
        return value
    def choose(day,theme):
        index=0 if day else 1
        value=open_selector(day)
        option=next(o for o in value['ui']['preset_options'] if o['period']==index and o['theme']==theme)
        click_rect(option['bounds']); settle()
        value=state()
        assert value['ui']['selector_presets'][index]==theme,value
        return value
    def open_editor(day):
        value=open_selector(day)
        click_rect(value['ui']['edit_buttons'][0 if day else 1])
        return wait(lambda s:s['editor_open'] and s['editor_period']==(0 if day else 1),'palette editor opens from selector')
    try:
        initial=start(480)
        assert initial['period']=='day' and initial['active_bg']==initial['day_bg'],initial
        settings(); automatic(False)
        # This is the first RED assertion on the old package: its five ordinary
        # themes are collapsed, and its day/night cards remain visible.
        value=assert_groups(False)
        capture(driver,out/'settings-manual.png')
        check('unchecked-shows-five-themes-and-hides-day-night',state=value)
        for theme in (0,1,2,3,10):
            click_stock(theme); before=state()
            command(cmd='theme_schedule',action='clock',minute=960); settle()
            value=state()
            assert value['mode']==0 and value['theme']==theme,value
            assert value['active_bg']==before['active_bg'] and value['active_accent']==before['active_accent'],value
            assert_groups(False)
            check('manual-theme-ignores-clock-'+str(theme),state=value)
        manual=state(); close_settings(); stop()
        value=start(480)
        assert value['mode']==0 and value['theme']==manual['theme'] and value['active_bg']==manual['active_bg'],value
        check('unchecked-manual-choice-survives-restart',state=value)
        settings(); off_bounds=state()['auto_toggle_bounds']; automatic(True); clock(480)
        value=assert_groups(True)
        assert value['auto_toggle_bounds'][:2]==off_bounds[:2],('checkbox moved on toggle',value['auto_toggle_bounds'],off_bounds)
        capture(driver,out/'settings-auto-day.png')
        check('checked-hides-five-themes-and-shows-two-selectors',state=value)
        for day,choices in ((True,(3,10,1,2,0)),(False,(1,2,0,3,10))):
            for theme in choices:
                other=night_file if day else day_file
                before=snapshot(other)
                value=choose(day,theme)
                assert snapshot(other)==before,'choosing one preset changed the other palette'
                check(('day' if day else 'night')+'-preset-'+str(theme),state=value)
        # Keep dark-by-day/light-by-night across restart, with correct preset names.
        expected_palettes=(snapshot(day_file),snapshot(night_file))
        close_settings(); stop(); value=start(480)
        assert value['active_bg']==value['day_bg'] and not value['day_warning'] and not value['night_warning'],value
        settings(); assert_groups(True)
        assert state()['ui']['selector_presets']==[0,10],state()
        assert (snapshot(day_file),snapshot(night_file))==expected_palettes
        clock(960); check('dark-day-light-night-presets-survive-restart',state=state())
        clock(480)
        open_selector(True); capture(driver,out/'day-theme-menu.png')
        # Close this menu by selecting its existing choice through the real item.
        option=next(o for o in state()['ui']['preset_options'] if o['period']==0 and o['theme']==0)
        click_rect(option['bounds']); settle()
        open_selector(False); capture(driver,out/'night-theme-menu.png')
        option=next(o for o in state()['ui']['preset_options'] if o['period']==1 and o['theme']==10)
        click_rect(option['bounds']); settle()
        night_before=snapshot(night_file)
        open_editor(True); draft('toolbarAccent',[.2,.42,.64,1])
        capture(driver,out/'editor-day.png'); click('保存配色')
        wait(lambda s:not s['editor_open'],'day saved'); settle()
        assert snapshot(night_file)==night_before,'day save touched night'
        day_saved=snapshot(day_file)
        colors=json.loads(day_saved[0])['colors']
        assert len(colors)==113
        assert all(abs(a-b)<1e-6 for a,b in zip(colors['toolbarAccent'],[.2,.42,.64,1])),colors['toolbarAccent']
        assert state()['ui']['selector_presets'][0]==7,'edited palette must say custom'
        check('day-edit-saves-exact-rgba-and-keeps-night',state=state())
        open_editor(True); draft('toolbarAccent',[.9,.1,.2,1]); click('取消修改')
        wait(lambda s:not s['editor_open'],'cancel closes')
        assert snapshot(day_file)==day_saved,'cancel changed day file'
        check('cancel-keeps-saved-day-palette',state=state())
        open_editor(True); draft('mainWindowBG',[.1,.1,.1,1]); click('保存配色')
        wait(lambda s:not s['editor_open'],'dark day background saved'); settle()
        day_saved=snapshot(day_file)
        colors=json.loads(day_saved[0])['colors']
        assert all(abs(a-b)<1e-6 for a,b in zip(colors['mainWindowBG'],[.1,.1,.1,1]))
        assert snapshot(night_file)==night_before,'dark day edit touched night'
        check('dark-day-background-saves',state=state())
        open_editor(False); draft('toolbarAccent',[.65,.42,.86,1]); draft('mainWindowBG',[.94,.94,.94,1])
        command(cmd='theme_schedule',action='clock',minute=960); settle()
        value=state()
        assert value['editor_open'] and value['active_accent']!='ff6f98c1',value
        capture(driver,out/'editor-night.png'); click('保存配色')
        wait(lambda s:not s['editor_open'],'night saved'); settle()
        assert snapshot(day_file)==day_saved,'night save touched day'
        night_saved=snapshot(night_file)
        colors=json.loads(night_saved[0])['colors']
        assert all(abs(a-b)<1e-6 for a,b in zip(colors['toolbarAccent'],[.65,.42,.86,1])),colors['toolbarAccent']
        assert all(abs(a-b)<1e-6 for a,b in zip(colors['mainWindowBG'],[.94,.94,.94,1]))
        check('light-night-edit-saves-exact-rgba-and-keeps-day',state=state())
        for minute in (479,480,959,960,0,1439,480):
            value=clock(minute); assert_groups(True)
            check('clock-boundary-'+str(minute),state=value)
        for theme in (0,1,2,3,10):
            command(cmd='set_theme',theme=theme)
            value=clock(480)
            assert snapshot(day_file)==day_saved and snapshot(night_file)==night_saved
            check('automatic-restores-current-period-'+str(theme),state=value)
        clock(960); capture(driver,out/'settings-auto-night.png')
        close_settings(); capture(driver,out/'library-night.png'); stop()
        value=start(480)
        assert value['mode']==1 and value['active_accent']==value['day_accent'],value
        assert snapshot(day_file)==day_saved and snapshot(night_file)==night_saved
        settings(); assert_groups(True)
        assert state()['ui']['selector_presets']==[7,7],'custom labels must survive restart'
        capture(driver,out/'settings-auto-custom.png')
        check('edited-palettes-and-automatic-mode-survive-restart',state=state())
        for day in (True,False):
            before_files=(snapshot(day_file),snapshot(night_file))
            open_editor(day); draft('toolbarAccent',[.25,.45,.65,1]); stop()
            value=start(960 if day else 480)
            assert value['mode']==1 and value['active_accent']==value[('night' if day else 'day')+'_accent'],value
            assert (snapshot(day_file),snapshot(night_file))==before_files,'unsaved draft changed saved palette'
            settings(); assert_groups(True)
            check('exit-during-'+('day' if day else 'night')+'-preview-keeps-saved-palette',state=value)
        # Compatibility: unchecking freezes the current palette until an ordinary
        # theme is chosen; neither the clock nor a restart may override that.
        for day in (True,False):
            automatic(True); clock(480 if day else 960); automatic(False)
            value=assert_groups(False)
            before=(value['active_bg'],value['active_accent'])
            command(cmd='theme_schedule',action='clock',minute=960 if day else 480); settle()
            assert (state()['active_bg'],state()['active_accent'])==before
            close_settings(); stop()
            value=start(960 if day else 480)
            assert value['mode']!=1 and (value['active_bg'],value['active_accent'])==before,value
            settings(); assert_groups(False)
            click('恢复默认主题'); value=settle()
            assert value['mode']==0 and value['theme']==0,('default theme was overridden',value)
            close_settings(); stop(); value=start(960 if day else 480)
            assert value['mode']==0 and value['theme']==0,value
            settings(); assert_groups(False)
            check(('day' if day else 'night')+'-unchecked-freeze-and-restore-default-survive-restart',state=value)
        # Leave this independently isolated experience profile in the simple
        # manual layout with the original five themes visible.
        click_stock(2); capture(driver,out/'settings-manual-final.png')
        close_settings(); command(cmd='theme_schedule',action='clock',minute=-1); settle(); stop()
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
            report['not_covered']=['User visual acceptance','Physical sleep/resume and OS timezone changes; equivalent wall-clock jumps tested','Every individual color-picker gesture; draft-color seam used with real open/save/cancel controls']
            (out/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',required=True,type=Path)
    parser.add_argument('--isolation-root',required=True,type=Path)
    parser.add_argument('--library',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--qa-narration',action='store_true',help='Also verify the optional local-only QA narration panel')
    run(parser.parse_args())
